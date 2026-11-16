#!/usr/bin/env python3
"""Two machines' records of pure-run (recorded-pure-run's logs): the
modules each compiled, and for every run both made (the same module, the
same arguments and input), whether output, errors and exit are the same.

compare.py A.jsonl B.jsonl           a JSON summary, and each run that differs
compare.py --whole A.jsonl B.jsonl   the same, and exit 1 unless both made the same runs and none differs"""
import json
import sys
from collections import Counter


def load(path):
    runs = [json.loads(line) for line in open(path)]
    return runs, Counter((r['module'], r['args'], r['input']) for r in runs)


whole = '--whole' in sys.argv[1:]
paths = [arg for arg in sys.argv[1:] if arg != '--whole']
(a, keys_a), (b, keys_b) = load(paths[0]), load(paths[1])
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
if whole and (differ or keys_a != keys_b):
    print(json.dumps({'runs_only_a': sum((keys_a - keys_b).values()), 'runs_only_b': sum((keys_b - keys_a).values())}))
    sys.exit(1)
