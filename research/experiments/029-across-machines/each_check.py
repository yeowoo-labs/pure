#!/usr/bin/env python3
"""Pure's tests/check.py one check at a time (research 029): the statements
of its main, each run alone in a temporary directory of its own, going on
past one that fails, with its time. A JSON line each.

each_check.py AUP [NAME ...]   all, or the checks whose report key has a
                               NAME in it; each line says when it began and
                               ended on the system's monotonic clock (as
                               clock.py's lines do)"""
import argparse
import ast
import json
import sys
import tempfile
import time
import traceback
from pathlib import Path

PURE = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(PURE / 'tests'))
import check  # noqa: E402

tree = ast.parse((PURE / 'tests' / 'check.py').read_text())
main = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'main')
block = next(node for node in ast.walk(main) if isinstance(node, ast.With))
aup = Path(sys.argv[1]).resolve()
check.EDITOR = aup.parents[1] / 'examples' / 'editor.aup'
wanted = sys.argv[2:]
for statement in block.body:
    if not isinstance(statement, ast.Assign):
        continue
    names = [ast.unparse(target) for target in statement.targets]
    if not any('report[' in n for n in names):  # the check's own (its directory), not a check
        continue
    if wanted and not any(w in n for w in wanted for n in names):
        continue
    began = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='pure-check-') as directory:
        space = dict(vars(check), aup=aup, pure=PURE, work=Path(directory), report={},
                     args=argparse.Namespace(seeds=4))
        try:
            exec(compile(ast.Module(body=[statement], type_ignores=[]), 'check.py', 'exec'), space)
            line = {'check': names[0], 'passed': True, 'report': space['report']}
        except Exception as failure:  # noqa: BLE001 - every failure is reported, and the next check runs
            line = {'check': names[0], 'passed': False, 'failure': repr(failure)[:600],
                    'where': traceback.format_exc().splitlines()[-3][:300]}
    line['seconds'] = round(time.monotonic() - began, 1)
    line['monotonic'] = [round(began, 2), round(time.monotonic(), 2)]
    print(json.dumps(line, ensure_ascii=False), flush=True)
