#!/usr/bin/env python3
"""The pure-runs of aup's checks of docs/design/018 (transients and
computing: the examples routes.aup and numbers.aup against their models),
each recorded by 029's recorded-pure-run, so two machines' records compare
run by run (029's compare.py).

record.py AUP_ROOT PURE_ROOT OUT.jsonl   the checks' report on stdout"""
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

aup_root, pure_root, out = (Path(a).resolve() for a in sys.argv[1:4])
subprocess.run(['make', '-s', '-C', str(pure_root), 'build/pure-run'], check=True)
subprocess.run(['make', '-s', '-C', str(aup_root), 'build/aup'], check=True)
out.unlink(missing_ok=True)
os.environ['PURE_RUN'] = str(pure_root / 'build' / 'pure-run')
os.environ['PURE_RUN_LOG'] = str(out)
recorded = pure_root / 'research' / 'experiments' / '029-across-machines' / 'recorded-pure-run'
spec = importlib.util.spec_from_file_location('check', aup_root / 'tests' / 'check.py')
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)
with tempfile.TemporaryDirectory(prefix='aup-check-') as directory:
    work = Path(directory)
    report = {'transients': check.transients(aup_root / 'build' / 'aup', recorded, work),
              'computing': check.computing(aup_root / 'build' / 'aup', recorded, work)}
print(json.dumps(report))
