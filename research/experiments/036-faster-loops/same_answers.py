#!/usr/bin/env python3
"""Whether builds answer the bank's queries alike (research 121): each
pure-serve loaded as 025's count.py loads it, each query registered and asked
once; prints, per query, whether the answers' bytes are the same.

same_answers.py SERVE SERVE...
"""
import http.client, json, select, subprocess, sys, tempfile, hashlib
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'research/experiments/025-i64-cost'))
import count  # noqa
bank = count.bank
aup = ROOT.parent / 'aup/build/aup'
views = {name: view for name, (view, _) in bank.QUERIES.items()} | count.TABLES
work = Path(tempfile.mkdtemp(prefix='pure-same-', dir='/dev/shm'))
module = work / 'bank.pmod'
subprocess.run([str(aup), 'build', str(bank.HERE / 'bank.aup'), '-o', str(module)], check=True)
compiled = {}
for name, view in views.items():
    (work / f'{name}.aup').write_text(view)
    subprocess.run([str(aup), 'query', str(bank.HERE / 'bank.aup'), str(work / f'{name}.aup'), '-o', str(work / f'{name}.pmod')], check=True)
    compiled[name] = (work / f'{name}.pmod').read_bytes()
answers = {}
for k, serve in enumerate(sys.argv[1:]):
    p = subprocess.Popen(['taskset', '-c', '0-2', serve, '--module', str(module), '--store', str(work / f's{k}'), '--port', '0'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        select.select([p.stdout], [], [], 10)
        port = json.loads(p.stdout.readline())['port']
        count.load(port)
        c = http.client.HTTPConnection('127.0.0.1', port, timeout=120)
        for name, b in compiled.items():
            c.request('PUT', f'/queries/{name}', b); r = c.getresponse(); r.read()
            c.request('GET', f'/queries/{name}'); r = c.getresponse(); data = r.read()
            answers.setdefault(name, []).append((r.status, len(data), hashlib.sha256(data).hexdigest()[:16], data[:120]))
    finally:
        p.terminate(); p.communicate(timeout=60)
for name, got in answers.items():
    print(name, 'same' if len({g[2] for g in got}) == 1 else 'DIFFERENT', [g[:3] for g in got])
    if len({g[2] for g in got}) != 1:
        for g in got: print('   ', g[3])
