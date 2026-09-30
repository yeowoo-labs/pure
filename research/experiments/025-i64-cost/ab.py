#!/usr/bin/env python3
"""The bank's four queries on two pure-serves at once, the same rows loaded
into each, asked in turns (the order flipped every round): ab.py BEFORE AFTER
ROUNDS ASKS. Prints each query's median of the rounds' medians, and in how
many rounds AFTER was slower. One process a side: what differs between two
processes (where code and data land) stays in the comparison, so run it
again on fresh processes (ab-sessions.sh) before believing a percent."""
import http.client, json, random, select, shutil, statistics, subprocess, sys, tempfile, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'benchmarks' / 'bank'))
import queries as q
aup = str(ROOT.parent / 'aup' / 'build' / 'aup')
serves = {'before': sys.argv[1], 'after': sys.argv[2]}
rounds, asks = int(sys.argv[3]), int(sys.argv[4])
work = Path(tempfile.mkdtemp(prefix='pure-ab-', dir='/dev/shm'))
processes = {}
try:
    module = work / 'bank.pmod'
    subprocess.run([aup, 'build', str(q.HERE / 'bank.aup'), '-o', str(module)], check=True)
    compiled = {}
    for name, (view, _) in q.QUERIES.items():
        (work / f'{name}.aup').write_text(view)
        subprocess.run([aup, 'query', str(q.HERE / 'bank.aup'), str(work / f'{name}.aup'), '-o', str(work / f'{name}.pmod')], check=True)
        compiled[name] = (work / f'{name}.pmod').read_bytes()
    ports = {}
    for version, serve in serves.items():
        p = subprocess.Popen(['taskset', '-c', '0-2', serve, '--module', str(module), '--store', str(work / f'store-{version}'),
                              '--port', '0', '--timeout-ms', '600000'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        processes[version] = p
        select.select([p.stdout], [], [], 10)
        ports[version] = json.loads(p.stdout.readline())['port']
    accounts, branches, transfers_n = 100000, 10, 20000
    rng = random.Random(9)
    tellers = 10 * branches
    transfers = [(rng.randint(1, accounts), rng.randint(1, tellers), rng.randint(1, branches), rng.randint(-5000, 5000))
                 for _ in range(transfers_n)]
    for version, port in ports.items():
        q.post_all(port, '/inputs/branch', [json.dumps({'bid': b}).encode() for b in range(1, branches + 1)], 1)
        q.post_all(port, '/inputs/teller', [json.dumps({'tid': t, 'bid': (t - 1) // 10 + 1}).encode() for t in range(1, tellers + 1)], 1)
        q.post_all(port, '/inputs/account', [json.dumps({'aid': a, 'bid': (a - 1) % branches + 1}).encode()
                                             for a in range(1, accounts + 1)])
        q.post_all(port, '/inputs/transfer', [json.dumps({'aid': a, 'tid': t, 'bid': b, 'delta': d}).encode()
                                              for a, t, b, d in transfers], 1)
    clients = {v: http.client.HTTPConnection('127.0.0.1', port, timeout=120) for v, port in ports.items()}
    for v, client in clients.items():
        for name, data in compiled.items():
            client.request('PUT', f'/queries/{name}', data)
            r = client.getresponse(); r.read(); assert r.status == 200
    answers = {}
    medians = {v: {name: [] for name in q.QUERIES} for v in serves}
    for r in range(rounds):
        order = ['before', 'after'] if r % 2 == 0 else ['after', 'before']
        for name in q.QUERIES:
            for v in order:
                times = []
                for _ in range(asks):
                    began = time.perf_counter()
                    clients[v].request('GET', f'/queries/{name}')
                    res = clients[v].getresponse(); data = res.read()
                    times.append(time.perf_counter() - began)
                    assert res.status == 200
                answers.setdefault(name, data)
                assert answers[name] == data, name  # both answer alike
                medians[v][name].append(statistics.median(times) * 1e3)
    for name in q.QUERIES:
        b, a = medians['before'][name], medians['after'][name]
        wins = sum(1 for x, y in zip(b, a) if y > x)
        print(json.dumps({'query': name, 'before_ms': round(statistics.median(b), 3), 'after_ms': round(statistics.median(a), 3),
                          'change_pct': round((statistics.median(a) / statistics.median(b) - 1) * 100, 2),
                          'rounds_after_slower': wins, 'rounds': len(b)}), flush=True)
finally:
    for p in processes.values():
        p.terminate(); p.communicate(timeout=60)
    shutil.rmtree(work)
