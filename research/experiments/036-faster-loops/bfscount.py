#!/usr/bin/env python3
"""A search's user-mode instructions and cycles, core only (research 121):
each bfs-run (BFS_WAIT) loads the graph, then 025's pcount (built in
PCOUNT_DIR from 025's pcount.c) counts its seven searches.

bfscount.py PCOUNT_DIR BFS_RUN...   (the graph: ~/.cache/pure-036/graph.pmod, runs.sh build)
"""
import subprocess, sys, os, json
SP = sys.argv[1]
for binary in sys.argv[2:]:
    env = dict(os.environ, BFS_WAIT='1')
    p = subprocess.Popen([binary, os.path.expanduser('~/.cache/pure-036/graph.pmod'), '100000', '1000000', '7'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, text=True)
    assert p.stderr.readline().strip() == 'loaded'
    c = subprocess.Popen([f'{SP}/pcount', str(p.pid)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert c.stderr.readline().strip() == 'ready'
    p.stdin.write('\n'); p.stdin.flush()
    out = p.stdout.read().strip().splitlines()[-1]; p.wait()
    counts = json.loads(c.communicate('')[0])
    print(binary.split('/')[-2], out, {k: round(v / 7 / 1e6, 2) for k, v in counts.items()}, 'M per search')
