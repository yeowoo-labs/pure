#!/usr/bin/env python3
"""Research 049, stage 1 (030's step 3: the emitter's bar; the user's decision of 2026-10-25, (b) with 049's bar kept):
SHA-256's handler (aup's examples/sha256.aup, one block, no function) emitted as C from its verified code by emit.py,
the exact rung (today's values: numbers as binary64, every i64 operation checked; the runtime's lists and bytes; the
work counted as the executor counts it), built with cc -O2 into core/module.c (PURE_EMITTED), against the executor on
the same build otherwise: sha-cost.c's answers (307 digests against C's, six events cut short by a small work budget)
and its machine instructions and cycles a block (user mode), C's beside them.

  run.py OUT.jsonl SIZE ROUNDS        builds dump, the executor's sha-cost and the emitted one, appends a JSON line each

Written before the emitted code ran (what would break the stage):
- Answers: all 307 digests equal C's on both builds, and the six cut events end alike on both (result, fault, the
  instruction it is at, the digest left), or the emitted code is wrong whatever it costs.
- Cost: the emitted handler's cycles a block at most a quarter of the executor's. Predicted 5-8 times fewer: the work is
  the dispatch of 16,476 instructions a block (045's exact rung was 4.4 times the executor on UAX #14, a list-heavy
  app), the lists' reads and writes (some 450 a block through the generic list API) staying as they are.
- Covered: every instruction inside the block loop written out (none handed to the executor's step); what is handed
  on is the message's once (the constants' appends, the padding, the digest's text).
Not a bar of 049 (T_b against T_a): this is T_a's first rung, its absolute numbers.

Found after the first runs (2026-10-25): of the 56k machine instructions a block that writing the i64 conversions out
took away (109,671 to 53,238), some 35.5k were the call to pure_cell_as and some 21k cc's folding of the constants it
then saw (a shift's count, 32 - n, the guards on them): with every constant hidden from cc (EMIT_OPAQUE=1, an empty asm
on each), 74,114 and 22,537 cycles. An emitter that folds nothing meets the hidden number; the folded one needs the
constants as facts (the verifier's, as its heights are) or folding of its own. Which one T_b is held to is written
before T_b runs."""
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PURE = HERE.parents[2]
AUP = Path(os.environ.get('AUP', PURE.parent / 'aup')) / 'build' / 'aup'
WORK = Path(os.environ.get('EMIT_WORK', '/dev/shm/pure-049'))
CORE = PURE / 'core'
CFLAGS = ['-O2', '-std=c17', '-Wall', '-Wextra', f'-I{CORE}']
OBJECTS = [PURE / 'build' / 'obj' / 'core' / f'{n}.o' for n in
           ('module-batch', 'module-jit', 'module-list', 'module-save', 'module-text', 'module-store', 'flow')] + \
          [PURE / 'build' / 'obj' / 'store' / f'{n}.o' for n in ('store-tree', 'store-log')]


def sh(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True, **kw)


def main():
    out, size, rounds = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    WORK.mkdir(parents=True, exist_ok=True)
    module = WORK / 'sha256.pmod'
    sh(AUP, 'build', PURE.parent / 'aup' / 'examples' / 'sha256.aup', '-o', module)
    sh('cc', *CFLAGS, HERE / 'dump.c', PURE / 'build' / 'libpure.a', '-lm', '-pthread', '-o', WORK / 'dump')
    (WORK / 'sha256.json').write_text(sh(WORK / 'dump', module).stdout)
    covered = json.loads(sh(sys.executable, HERE / 'emit.py', WORK / 'sha256.json', '0', WORK / 'emitted.c').stdout)
    sh('cc', *CFLAGS, HERE / 'sha-cost.c', PURE / 'build' / 'libpure.a', '-lm', '-pthread', '-o', WORK / 'sha-executor')
    sh('cc', *CFLAGS, '-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-maybe-uninitialized', '-c', WORK / 'emitted.c', '-o', WORK / 'emitted.o')
    sh('cc', *CFLAGS, HERE / 'sha-cost.c', WORK / 'emitted.o', *OBJECTS, '-lm', '-pthread', '-o', WORK / 'sha-emitted')
    lines = {}
    for way in ('executor', 'emitted'):
        done = sh(WORK / f'sha-{way}', module, size, rounds, env=dict(os.environ, PURE_JIT='0'))
        lines[way] = json.loads(done.stdout.strip().splitlines()[-1])
        line = {'stage': 1, 'way': way, **lines[way]}
        if way == 'emitted':
            line['covered'] = covered
            line['constants_hidden'] = bool(os.environ.get('EMIT_OPAQUE'))  # emit.py's EMIT_OPAQUE: cc folds none
        with open(out, 'a') as f:
            f.write(json.dumps(line) + '\n')
        print(json.dumps(line), flush=True)
    same = lines['executor']['cut'] == lines['emitted']['cut']
    e, x = lines['emitted'], lines['executor']
    print(json.dumps({'cut_alike': same, 'cycles_ratio': round(x['pure_cycles_per_block'] / e['pure_cycles_per_block'], 2),
                      'instructions_ratio': round(x['pure_instructions_per_block'] / e['pure_instructions_per_block'], 2),
                      'to_c_cycles': round(e['pure_cycles_per_block'] / e['c_cycles_per_block'], 1)}))


if __name__ == '__main__':
    main()
