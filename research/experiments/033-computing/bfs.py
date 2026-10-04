#!/usr/bin/env python3
"""A breadth first search in one event (docs/design/018 stage 2, research
118). A random graph of NODES nodes and EDGES edges (from and to uniform,
no loops, each pair once; seed 118) goes into graph.aup on pure-serve
(store in RAM, cores 0-2) as rows; then SEARCHES searches from random
nodes, each one event (POST /inputs/route), timed by the client (the
request on a kept connection; the event is nearly all of it), ROUNDS
times each. The same searches run in C (bfs.c, adjacency as offsets) and
in CPython (lists and a deque). All three must find the same hops and
reach the same nodes.

bfs.py PURE_SERVE AUP NODES EDGES SEARCHES ROUNDS   a JSON line per search and system, then medians
"""
import collections
import http.client
import json
import random
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    serve, aup = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    nodes, edges_wanted, searches, rounds = (int(a) for a in sys.argv[3:7])
    rng = random.Random(118)
    pairs = set()
    while len(pairs) < edges_wanted:
        a, b = rng.randrange(nodes), rng.randrange(nodes)
        if a != b:
            pairs.add((a, b))
    edges = sorted(pairs)
    out = [[] for _ in range(nodes)]
    for a, b in edges:
        out[a].append(b)
    queries = [(rng.randrange(nodes), rng.randrange(nodes)) for _ in range(searches)]
    work = Path(tempfile.mkdtemp(prefix='pure-033-', dir='/dev/shm'))
    try:
        with open(work / 'graph.bin', 'wb') as f:
            f.write(struct.pack('<II', nodes, len(edges)))
            for a, b in edges:
                f.write(struct.pack('<II', a, b))
        subprocess.run(['cc', '-O2', '-o', str(work / 'bfs'), str(HERE / 'bfs.c')], check=True)
        c_runs = subprocess.run(['taskset', '-c', '1', str(work / 'bfs'), str(work / 'graph.bin')] +
                                [str(v) for q in queries for v in q], capture_output=True, text=True, check=True).stdout
        found = {}
        for line in c_runs.splitlines():
            r = json.loads(line)
            found[(r['from'], r['to'])] = (r['hops'], r['reached'])
            print(json.dumps(r), flush=True)
        times = {'c': [json.loads(line)['us'] for line in c_runs.splitlines()], 'python': [], 'pure': []}
        for s, t in queries:
            runs = []
            for _ in range(rounds):
                began = time.perf_counter()
                depth = {s: 0}
                queue = collections.deque([s])
                while queue:
                    v = queue.popleft()
                    for w in out[v]:
                        if w not in depth:
                            depth[w] = depth[v] + 1
                            queue.append(w)
                runs.append((time.perf_counter() - began) * 1e6)
            assert (depth.get(t, -1), len(depth)) == found[(s, t)], (s, t)
            times['python'].append(statistics.median(runs))
            print(json.dumps({'system': 'python', 'from': s, 'to': t, 'us': round(statistics.median(runs), 1)}), flush=True)
        subprocess.run([str(aup), 'build', str(HERE / 'graph.aup'), '-o', str(work / 'graph.pmod')], check=True)
        host = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--module', str(work / 'graph.pmod'), '--store',
                                 str(work / 'store'), '--port', '0'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        port = json.loads(host.stdout.readline())['port']
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=600)

        def request(method, path, body=b''):
            connection.request(method, path, body, {'Content-Length': str(len(body))})
            answer = connection.getresponse()
            data = answer.read()
            assert answer.status == 200, (path, answer.status, answer.getheader('X-Pure-Error'), data[:200])
            return data

        request('POST', f'/inputs/size?nodes={nodes}')
        for k in range(0, len(edges), 80000):
            request('POST', '/inputs/load', b''.join(struct.pack('<dd', a * 1048576 + b, b) for a, b in edges[k:k + 80000]))
        for s, t in queries:
            runs = []
            for _ in range(rounds):
                began = time.perf_counter()
                request('POST', f'/inputs/route?from={s}&to={t}')
                runs.append((time.perf_counter() - began) * 1e6)
            got = (int(request('GET', '/views/far')), int(request('GET', '/views/found')))
            assert got == found[(s, t)], (s, t, got, found[(s, t)])
            times['pure'].append(statistics.median(runs))
            print(json.dumps({'system': 'pure', 'from': s, 'to': t, 'hops': got[0], 'reached': got[1],
                              'us': round(statistics.median(runs), 1)}), flush=True)
        host.terminate()
        host.communicate(timeout=60)
        medians = {k: round(statistics.median(v), 1) for k, v in times.items()}
        print(json.dumps({'nodes': nodes, 'edges': len(edges), 'median_us': medians,
                          'pure_over_c': round(medians['pure'] / medians['c'], 1),
                          'pure_over_python': round(medians['pure'] / medians['python'], 2)}), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
