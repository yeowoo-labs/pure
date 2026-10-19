#!/usr/bin/env python3
"""Research 047: what today's store writes for a workload, by file, as the base the leaf-commit model is held to.
046's owner (pure-run --store, the shim shipping every cut to a follower, its journal kept) takes a workload's messages
at a commit grouping (pure-run --batch N: N messages a save); the journal's ops give the bytes written, by file: the
list log (log-N), the lists' images, the tree's commits (pure.tree) and its compactions (.pending-*), zeros apart. A
workload with a setup (the bank's branches, tellers and accounts) runs twice, setup alone and setup then the work, and
the work's bytes are the difference.

  baseline.py entries MESSAGES        appends of one number (046's entries.aup)
  baseline.py bank ACCOUNTS TRANSFERS the bank (benchmarks/bank/bank.aup): 10 branches, 100 tellers, ACCOUNTS accounts,
                                      then TRANSFERS transfers (uniform aid, tid, bid; a whole delta in -5000..5000)
One JSON line a grouping (1, 16, 256)."""
import collections
import json
import random
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / '046-commit-replica'))
import prefixes  # noqa: E402  (046's journal reader)
import run as harness  # noqa: E402

PURE = HERE.parents[2]


def entries_commands(n):
    return [harness.message(i) for i in range(n)], 0


def bank_commands(accounts, transfers, seed=47):
    rng = random.Random(seed)
    setup = [f'message branch {struct.pack("<I", b).hex()}\n' for b in range(10)]
    setup += [f'message teller {struct.pack("<II", t, t % 10).hex()}\n' for t in range(100)]
    setup += [f'message account {struct.pack("<II", a, a % 10).hex()}\n' for a in range(accounts)]
    work = [f'message transfer {struct.pack("<IIId", rng.randrange(accounts), rng.randrange(100), rng.randrange(10), float(rng.randint(-5000, 5000))).hex()}\n'
            for _ in range(transfers)]
    return setup + work, len(setup)


def written(journal):
    """Bytes written by file kind (zeros apart), and the number of syncs."""
    out, zeros, syncs = collections.Counter(), collections.Counter(), 0
    for kind, path, rest, _ in prefixes.ops_of(journal.read_bytes()):
        if kind == prefixes.OP_SYNC:
            syncs += 1
        if kind != prefixes.OP_WRITE:
            continue
        name = ('compaction' if path.startswith('.pending') else 'image' if 'image' in path else
                'log' if path.startswith('log-') else 'tree' if path == 'pure.tree' else path)
        data = rest[12:]
        out[name] += len(data)
        if not any(data):
            zeros[name] += len(data)
    return out, zeros, syncs


def one(keys, module, commands, batch):
    journal = harness.WORK / 'journal'
    harness.ENV['SHIM_JOURNAL'] = str(journal)
    r = harness.Run(keys, module=module, extra=('--batch', str(batch)) if batch > 1 else ())
    try:
        for c in commands:
            r.owner.stdin.write(c)
        r.owner.stdin.close()
    except BrokenPipeError:
        pass
    r.owner.wait()
    r.reader.join(timeout=30)
    r.shipper.communicate(timeout=60)
    r.stop_follower()
    del harness.ENV['SHIM_JOURNAL']
    return written(journal), r.done


def main():
    keys = harness.build()
    what = sys.argv[1]
    if what == 'entries':
        module, (commands, setup) = 'entries', entries_commands(int(sys.argv[2]))
    else:
        subprocess.run([str(harness.AUP), 'build', str(PURE / 'benchmarks' / 'bank' / 'bank.aup'), '-o',
                        str(harness.WORK / 'bank.pmod')], check=True)
        module, (commands, setup) = 'bank', bank_commands(int(sys.argv[2]), int(sys.argv[3]))
    work = len(commands) - setup
    for batch in (1, 16, 256):
        (total, zeros, syncs), done = one(keys, module, commands, batch)
        if setup:
            (base, base_zeros, base_syncs), _ = one(keys, module, commands[:setup], batch)
            total, zeros, syncs = total - base, zeros - base_zeros, syncs - base_syncs
        print(json.dumps({'workload': what, 'messages': work, 'batch': batch, 'done': done,
                          'bytes_per_message': {k: round(v / work, 1) for k, v in sorted(total.items())},
                          'zero_bytes_per_message': {k: round(v / work, 1) for k, v in sorted(zeros.items())},
                          'list_bytes_per_message': round((total['log'] + total['image']) / work, 1),
                          'syncs_per_message': round(syncs / work, 3)}), flush=True)


if __name__ == '__main__':
    main()
