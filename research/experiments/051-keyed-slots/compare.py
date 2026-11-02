import json, statistics, sys
from collections import defaultdict
rows = [json.loads(l) for l in open(sys.argv[1])]
a, b = sys.argv[2], sys.argv[3]
by = defaultdict(list)
for r in rows: by[(r['work'], r['version'])].append(r['pure_median_ms'])
for work in dict.fromkeys(r['work'] for r in rows):
    x, y = by[(work, a)], by[(work, b)]
    c = statistics.median([r['c_median_ms'] for r in rows if r['work'] == work])
    print(f"{work:8} {a} {statistics.median(x):8.1f} ms  {b} {statistics.median(y):8.1f} ms  {b}/{a} {statistics.median(y)/statistics.median(x):.3f}"
          f"  (n={len(x)},{len(y)}; C {c:.1f} ms; agree {all(r['agree'] for r in rows if r['work'] == work)})")
