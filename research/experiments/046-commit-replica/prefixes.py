#!/usr/bin/env python3
"""Research 046: what a follower may hold when the owner's whole machine dies (run.py's kills seldom leave one with
part of a cut: a cut's records travel together). The owner takes EVENTS messages with the shim keeping every op it
shipped (SHIM_JOURNAL); then the ops are done again in order into a fresh directory, and at SAMPLES points (op
boundaries drawn at random, within cuts and at their ends, and the whole stream) a copy of what is there is opened by
pure-run as the owner (and around every rename: a compaction's or an image's install). Each must open without a fault, take
a message as an owner does, hold the first `total` messages' entries exactly (summed), hold
no fewer than at the sample before (a prefix never loses what a shorter one held), and the whole stream every message.

  prefixes.py EVENTS SAMPLES     one JSON line a sample, then one of the totals"""
import json
import os
import random
import shutil
import sys
import time
from pathlib import Path

import run as harness

OP_CREATE, OP_WRITE, OP_TRUNCATE, OP_ALLOCATE, OP_RENAME, OP_UNLINK, OP_MKDIR, OP_SYNC = range(1, 9)


def ops_of(journal):
    """The ops in order: (kind, path, rest) as bytes, and how many syncs (cuts) came before each."""
    out, at, cuts = [], 0, 0
    while at < len(journal):
        kind, n = journal[at], int.from_bytes(journal[at + 1:at + 3], 'little')
        path = journal[at + 3:at + 3 + n].decode()
        p = at + 3 + n
        if kind == OP_CREATE: end = p + 8
        elif kind == OP_WRITE: end = p + 12 + int.from_bytes(journal[p + 8:p + 12], 'little')
        elif kind == OP_TRUNCATE: end = p + 8
        elif kind == OP_ALLOCATE: end = p + 16
        elif kind == OP_RENAME: end = p + 2 + int.from_bytes(journal[p:p + 2], 'little')
        elif kind in (OP_UNLINK, OP_MKDIR): end = p + 4
        elif kind == OP_SYNC: end = p
        else: raise SystemExit(f'unknown op {kind} at {at}')
        out.append((kind, path, journal[p:end], cuts))
        cuts += kind == OP_SYNC
        at = end
    return out


def apply(root, op):
    kind, path, rest, _ = op
    target = root / path if path else root
    if kind == OP_CREATE:
        flags = int.from_bytes(rest[:4], 'little')
        fd = os.open(target, os.O_WRONLY | flags, int.from_bytes(rest[4:8], 'little'))
        os.close(fd)
    elif kind == OP_WRITE:
        offset, length = int.from_bytes(rest[:8], 'little'), int.from_bytes(rest[8:12], 'little')
        fd = os.open(target, os.O_WRONLY)
        os.pwrite(fd, rest[12:12 + length], offset)
        os.close(fd)
    elif kind == OP_TRUNCATE:
        os.truncate(target, int.from_bytes(rest, 'little'))
    elif kind == OP_ALLOCATE:
        fd = os.open(target, os.O_WRONLY)
        os.posix_fallocate(fd, int.from_bytes(rest[:8], 'little'), int.from_bytes(rest[8:16], 'little'))
        os.close(fd)
    elif kind == OP_RENAME:
        n = int.from_bytes(rest[:2], 'little')
        os.rename(target, root / rest[2:2 + n].decode())
    elif kind == OP_UNLINK:
        (os.rmdir if int.from_bytes(rest, 'little') & 0x200 else os.unlink)(target)
    elif kind == OP_MKDIR:
        target.mkdir(mode=int.from_bytes(rest, 'little'), exist_ok=True)


def main():
    events, samples = int(sys.argv[1]), int(sys.argv[2])
    keys = harness.build()
    journal = harness.WORK / 'journal'
    harness.ENV['SHIM_JOURNAL'] = str(journal)
    r = harness.Run(keys)
    r.feed(events)
    r.owner.wait()
    r.reader.join(timeout=10)
    r.shipper.communicate(timeout=60)
    r.stop_follower()
    del harness.ENV['SHIM_JOURNAL']
    ops = ops_of(journal.read_bytes())
    near = {i + d for i, op in enumerate(ops) if op[0] == OP_RENAME for d in range(-3, 4) if 0 < i + d <= len(ops)}
    picks = sorted(set(random.sample(range(1, len(ops)), min(samples, len(ops) - 1))) | near | {len(ops)})
    root, copy = harness.WORK / 'prefix', harness.WORK / 'prefix-open'
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir()
    done, last_total, failures, within = 0, -1, [], 0
    started = time.time()
    for pick in picks:
        while done < pick:
            apply(root, ops[done])
            done += 1
        mid = ops[pick - 1][0] != OP_SYNC  # the prefix ends inside a cut
        within += mid
        shutil.rmtree(copy, ignore_errors=True)
        shutil.copytree(root, copy)
        took = harness.opened(copy)
        total = int(took.get('total', -1)) if took.get('opened') else -1
        good = (took.get('opened') and took.get('fault') == 'none' and took.get('takes_a_message') and
                took.get('summed') == harness.summed(total) and
                total >= last_total and (pick < len(ops) or total == events))
        if not good:
            failures.append({'ops': pick, 'cut': ops[pick - 1][3], 'within_cut': mid, 'took': took, 'before': last_total})
        print(json.dumps({'ops': pick, 'of': len(ops), 'cut': ops[pick - 1][3], 'within_cut': mid, 'total': total,
                          'passed': bool(good)}), flush=True)
        last_total = max(last_total, total)
    renamed = {}
    for op in ops:
        if op[0] == OP_RENAME:
            to = op[2][2:].decode()
            renamed[to] = renamed.get(to, 0) + 1
    print(json.dumps({'run': 'prefixes', 'events': events, 'ops': len(ops), 'journal_bytes': journal.stat().st_size,
                      'renamed_to': renamed, 'samples': len(picks), 'near_renames': len(near), 'within_cuts': within, 'failed': len(failures), 'failures': failures[:5],
                      'seconds': round(time.time() - started, 1)}), flush=True)


if __name__ == '__main__':
    main()
