#!/usr/bin/env python3
"""Research 048: a function's list returned as a value, again and again (030's N4 in functions: the list moved, not
copied), on two builds of pure-run: an event calls a function 10,000 times, each making a list of two items and
returning it (returns.aup: items of two numbers, of two texts, or of a number and a list of two-number items, which a
second function made). User-mode machine instructions a call (041's icount, the least of three runs, an event of no
calls taken away), and the answer, which must be the same.

  returns.py BASE_PURE_RUN NEW_PURE_RUN OUT     appends a JSON line a kind of item to OUT"""
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PURE = HERE.parents[2]
AUP = Path(os.environ.get('AUP', PURE.parent / 'aup')) / 'build' / 'aup'


def counted(icount, binary, module, k, kind):
    command = f'message go {struct.pack("<II", k, kind).hex()}\n'
    least, answer = None, None
    for _ in range(3):
        r = subprocess.run([str(icount), str(binary), str(module)], input=command, capture_output=True, text=True,
                           env=dict(os.environ, PURE_JIT='0'), timeout=600)
        said = [l for l in r.stderr.splitlines() if l.startswith('{"instructions"')]
        assert r.returncode == 0 and said, r.stderr[-300:]
        instructions = json.loads(said[-1])['instructions']
        answer = json.loads(r.stdout.splitlines()[-1])['states']['s']
        least = instructions if least is None else min(least, instructions)
    return least, answer


def main():
    base, new, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    with tempfile.TemporaryDirectory(prefix='pure-returns-') as directory:
        work = Path(directory)
        icount, module = work / 'icount', work / 'returns.pmod'
        subprocess.run(['cc', '-O2', '-o', str(icount), str(PURE / 'research' / 'experiments' / '041-batches' / 'icount.c')], check=True)
        subprocess.run([str(AUP), 'build', str(HERE / 'returns.aup'), '-o', str(module)], check=True)
        for kind, name in ((0, 'numbers'), (1, 'texts'), (2, 'records holding lists')):
            line = {'items': name, 'calls': 10000}
            for label, binary in (('base', base), ('new', new)):
                none, _ = counted(icount, binary, module, 0, kind)
                some, answer = counted(icount, binary, module, 10000, kind)
                line[label] = {'instructions_a_call': round((some - none) / 10000, 1), 'answer': answer}
            line['change'] = round(line['new']['instructions_a_call'] / line['base']['instructions_a_call'] - 1, 4)
            with open(out, 'a') as f:
                f.write(json.dumps(line) + '\n')
            print(json.dumps(line), flush=True)


if __name__ == '__main__':
    main()
