#!/usr/bin/env python3
"""The pairs of runs.sh pairs as a table (research 118): each round's TPC-C
(transactions a second, CPU a transaction, the owner's core share) before
and after, the medians and the bank queries' median times.

pairs.py DATE   from DATE-tpcc-*.jsonl and DATE-queries-*.jsonl beside it
"""
import json
import statistics
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
day = sys.argv[1]


def tpcc(version, round_):
    for line in (HERE / f'{day}-tpcc-{version}-{round_}.jsonl').read_text().splitlines():
        r = json.loads(line)
        if r.get('step') == 'run' and r.get('system') == 'pure':
            owner = max((share for name, share in r['core_shares'].items() if 'owner' in name), default=None) \
                if isinstance(r['core_shares'], dict) else None
            return r['transactions_per_second'], r['cpu_us_per_transaction'], owner


def queries(version, round_):
    return {r['query']: r['pure_median_ms'] for r in map(json.loads, (HERE / f'{day}-queries-{version}-{round_}.jsonl')
                                                        .read_text().splitlines()) if 'query' in r}


rounds = sorted({int(p.stem.rsplit('-', 1)[1]) for p in HERE.glob(f'{day}-tpcc-before-*.jsonl')})
rows = {'before': [], 'after': []}
for round_ in rounds:
    b, a = tpcc('before', round_), tpcc('after', round_)
    rows['before'].append(b), rows['after'].append(a)
    print(json.dumps({'round': round_, 'tps': [b[0], a[0]], 'change': round((a[0] / b[0] - 1) * 100, 1),
                      'cpu_us': [b[1], a[1]], 'owner_share': [b[2], a[2]]}))
med = {v: statistics.median(t for t, _, _ in rows[v]) for v in rows}
cpu = {v: statistics.median(c for _, c, _ in rows[v]) for v in rows}
names = queries('before', rounds[0]).keys()
q = {v: {n: statistics.median(queries(v, r)[n] for r in rounds) for n in names} for v in rows}
print(json.dumps({'tps_median': [med['before'], med['after']], 'change': round((med['after'] / med['before'] - 1) * 100, 1),
                  'cpu_us_median': [cpu['before'], cpu['after']],
                  'queries_median_ms': {n: [q['before'][n], q['after'][n],
                                            round((q['after'][n] / q['before'][n] - 1) * 100, 1)] for n in names}}))
