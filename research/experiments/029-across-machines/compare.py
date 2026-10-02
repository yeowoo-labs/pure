#!/usr/bin/env python3
"""Two machines' records of pure-run (recorded-pure-run's logs): the
modules each compiled, and for every run both made (the same module, the
same arguments and input), whether output, errors and exit are the same.

compare.py A.jsonl B.jsonl   a JSON summary, and each run that differs"""
import json
import sys
from collections import Counter


def load(path):
    runs = [json.loads(line) for line in open(path)]
    return runs, Counter((r['module'], r['args'], r['input']) for r in runs)


(a, keys_a), (b, keys_b) = load(sys.argv[1]), load(sys.argv[2])
seen_a = {}
for r in a:
    seen_a.setdefault((r['module'], r['args'], r['input']), set()).add((r['output'], r['errors'], r['exit']))
both, same, differ = 0, 0, []
for r in b:
    key = (r['module'], r['args'], r['input'])
    if key not in seen_a:
        continue
    both += 1
    if (r['output'], r['errors'], r['exit']) in seen_a[key]:
        same += 1
    else:
        differ.append({'run': key, 'a': sorted(seen_a[key]), 'b': (r['output'], r['errors'], r['exit'])})
modules_a, modules_b = {r['module'] for r in a}, {r['module'] for r in b}
print(json.dumps({'runs_a': len(a), 'runs_b': len(b), 'modules_a': len(modules_a), 'modules_b': len(modules_b),
                  'modules_both': len(modules_a & modules_b), 'runs_both': both, 'runs_same': same,
                  'runs_differ': len(differ)}))
for d in differ[:20]:
    print(json.dumps(d))
