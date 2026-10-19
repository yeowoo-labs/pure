#!/usr/bin/env python3
"""The pure-runs of the checks that 027, 028 and 140 added, each recorded by recorded-pure-run, so that machines' records
compare run by run (compare.py): aup's checks of docs/design/018 (transients, computing: what 033's record.py records),
of 027's stages (bits and bytes, formats and texts, nested values, functions, the stored nest and keyed documents), of
028 (the bytes state) and of 140's frictions (bare lists, bytes compared, inner lists, rows read by position, the
frictions written away); and Pure's module_specs, the nine specs written in aup (SHA-256, JSON, JSON as a tree,
inflate, DNS, HTTP/1.1, UAX #14, RFC 6298, box layout), each on Pure's three paths. Pure's tests/specs are copied into
the temporary directory so that no machine's own paths are in a run's input.

record_all.py AUP_ROOT PURE_ROOT OUT.jsonl   the checks' report on stdout (MAKE: the make to build with)"""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

aup_root, pure_root, out = (Path(a).resolve() for a in sys.argv[1:4])
subprocess.run([os.environ.get('MAKE', 'make'), '-s', '-C', str(pure_root), 'build/pure-run'], check=True)
subprocess.run([os.environ.get('MAKE', 'make'), '-s', '-C', str(aup_root), 'build/aup'], check=True)
out.unlink(missing_ok=True)
os.environ['PURE_RUN'] = str(pure_root / 'build' / 'pure-run')
os.environ['PURE_RUN_LOG'] = str(out)
recorded = pure_root / 'research' / 'experiments' / '029-across-machines' / 'recorded-pure-run'
aup = aup_root / 'build' / 'aup'


def loaded(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


check = loaded('aup_check', aup_root / 'tests' / 'check.py')
report = {}
with tempfile.TemporaryDirectory(prefix='aup-check-') as directory:
    work = Path(directory)
    for name, run in (('transients', lambda: check.transients(aup, recorded, work)),
                      ('computing', lambda: check.computing(aup, recorded, work)),
                      ('bits_and_bytes', lambda: check.bits_and_bytes(aup, recorded, work)),
                      ('formats_and_texts', lambda: check.formats_and_texts(aup, recorded, work)),
                      ('nested_values', lambda: check.nested_values(aup, recorded, work)),
                      ('functions', lambda: check.functions(aup, recorded, work)),
                      ('bare_lists', lambda: check.bare_lists(aup, recorded, work)),
                      ('bytes_compared', lambda: check.bytes_compared(aup, recorded, work)),
                      ('inner_lists', lambda: check.inner_lists(aup, recorded, work)),
                      ('rows_at', lambda: check.rows_at(aup, recorded, work)),
                      ('frictions', lambda: check.frictions(aup, recorded, work)),
                      ('bytes_state', lambda: check.bytes_state(aup, recorded, work)),
                      ('stored_nested', lambda: check.stored_nested(aup, recorded, work, range(2))),
                      ('documents', lambda: check.documents_model(aup, recorded, work, range(2)))):
        report[name] = run()
pure_check = loaded('pure_check', pure_root / 'tests' / 'check.py')
with tempfile.TemporaryDirectory(prefix='pure-check-') as directory:
    work = Path(directory)
    view = work / 'pure'  # Pure as module_specs reads it: its pure-run recorded, its specs here
    (view / 'build').mkdir(parents=True)
    shutil.copy(recorded, view / 'build' / 'pure-run')
    shutil.copytree(pure_root / 'tests' / 'specs', view / 'tests' / 'specs')
    pure_check.EXAMPLES = aup_root / 'examples'
    report['module_specs'] = pure_check.module_specs(aup, view, work)
runs = sum(1 for _ in open(out))
report['runs'] = runs
print(json.dumps(report))
