#!/usr/bin/env python3
"""analyze.py A B FIRST LAST: TPC-C and bank-query medians of runs.sh pairs on 2026-10-26, A against B."""
import json, statistics, sys
from pathlib import Path
base = Path('research/experiments/040-against-c')
a, b, first, last = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
def tpcc(v, r):
    for l in open(base / f'2026-10-26-tpcc-{v}-{r}.jsonl'):
        d = json.loads(l)
        if d['step'] == 'run': return d['transactions_per_second'], d['cpu_us_per_transaction'], d['core_shares']['pure-owner']
def queries(v, r):
    return {json.loads(l)['query']: json.loads(l)['pure_median_ms'] for l in open(base / f'2026-10-26-queries-{v}-{r}.jsonl')}
rounds = range(first, last + 1)
ta = {r: tpcc(a, r) for r in rounds}; tb = {r: tpcc(b, r) for r in rounds}
for r in rounds: print(r, a, ta[r], b, tb[r], 'ratio %.4f' % (tb[r][0] / ta[r][0]))
ratios = [tb[r][0] / ta[r][0] for r in rounds]
print('tpcc: per-round ratio median %.4f, medians %s %d %s %d ratio %.4f; cpu/tx median %s %.1f %s %.1f; owner shares %s %s' % (
    statistics.median(ratios), a, statistics.median(t[0] for t in ta.values()), b, statistics.median(t[0] for t in tb.values()),
    statistics.median(t[0] for t in tb.values()) / statistics.median(t[0] for t in ta.values()),
    a, statistics.median(t[1] for t in ta.values()), b, statistics.median(t[1] for t in tb.values()),
    sorted(t[2] for t in ta.values()), sorted(t[2] for t in tb.values())))
qa = {r: queries(a, r) for r in rounds}; qb = {r: queries(b, r) for r in rounds}
for q in qa[first]:
    ma = statistics.median(qa[r][q] for r in rounds); mb = statistics.median(qb[r][q] for r in rounds)
    print('query %-10s %s %.3f ms  %s %.3f ms  %+.2f%%' % (q, a, ma, b, mb, 100 * (mb / ma - 1)))
