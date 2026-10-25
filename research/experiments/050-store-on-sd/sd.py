#!/usr/bin/env python3
"""Research 050: today's store on the Pi's SD card (ext4), timed by file, before anything is merged (030's step 4; the
design panel's critic, 2026-10-25: the one experiment that teaches the most for the least). Nothing in the store is
changed. 046's owner (pure-run --store, 046's shim preloaded) takes 047's workloads at a commit grouping (pure-run
--batch N), its store on the card: alone (SHIM_ALONE: no cut sent), or shipping every cut to 046's follower whose store
is on the same card (synchronous: the owner's sync waits for the follower's). The shim times the owner's own syncs by
the file they sync (log-N, pure.tree, a compaction's .pending file, an image, a directory); the follower its file and
directory fsyncs and each cut's whole chain; the card's journal commits are read from /proc/fs/jbd2 before and after.

  sd.py entries MESSAGES ROUNDS [FIRST]   046's entries-price (an append and a count a message), alone and shipped,
                                    at 1, 16 and 256 messages a save, the order turned each round (rounds FIRST on)
  sd.py bank ACCOUNTS TRANSFERS ROUNDS   047's bank (lists only, no tree commit), alone and shipped, a save a message;
                                    its setup alone run too, and taken away (as 047's baseline does)
  sd.py twice MESSAGES              entries alone at 1 a save twice, into two stores: compared by names and bytes
  sd.py follower MESSAGES ROUNDS [FIRST]  added after the first round (not in the bars below): entries at 1 a save,
                                    shipped to 046's follower as it is and to one that syncs as the owner's store does
                                    (FOLLOWER_SYNC=data: fdatasync a file, fsync a directory only after a name changed),
                                    in turns
  sd.py merged B0_PURE_RUN MESSAGES ROUNDS [FIRST]   added after the first round too: the owner alone at 1, 16 and
                                    256 a save on today's pure-run and on B0 (a build whose saves carry the lists' changes
                                    in the tree's commit, one fdatasync, no log entry: a latency prototype, nothing reads
                                    them back), in turns; and the bank on both. A save's time is the shim's, from one sync
                                    of pure.tree to the next. The bars for it are below the first ones. B0 is e5aeedf with
                                    b0.patch (beside this file).
One JSON line a run.

Written before the runs (the critic's bars and predictions, and one added):
(1) entries, 1 a save, alone: pure.tree's syncs take 40-50% of the owner's time (about 1.2 ms each). Shipped, the
    follower's chain is added, so the share is smaller there. The bank (lists only) has no tree sync: 0%.
(2) log-N's sync has a p50 near the tree's (about 1.2 ms). The card's journal commits: at most 20 for 1,000 log appends
    in a new segment, about 0 in a segment written again; over 100 and the fallocate hypothesis holds (an unwritten
    extent converted at the first write is metadata, so the sync carries a journal commit).
(3) Shipped: the follower's chain is at least the owner's sync for a cut, and a message's at least twice the owner's
    own syncs. The two stores share one card, so its flushes queue: worse than two machines.
(4) Compactions: about 6 in 20,000 messages at 1 a save, each a new file of about 1.09 MB of zeros; about 10 ms each
    on the owner (085), more on the follower (it writes and syncs the zeros too).
(5) Twice: the two owners' stores are the same in names and bytes (a pid is only in temporary names).
Broken: (1) below 20% alone, and store 1's merge (one commit, one fdatasync) is a gain in bytes and code, not in time;
(3) below the owner's sync, and "replication's cost is its flushes" is wrong; (5) a byte differs, and compaction is not
deterministic (store 3's follower that compacts by itself cannot stand).

B0's, written after the first round and before B0 ran: entries alone at 1 a save, B0's p50 save at most 0.6 of today's
(predicted 0.51: one sync of about 1.2 ms where there were two), syncs a message from 2.0 to 1.0 or fewer; the bank
(lists only, today one log sync a message) no slower than 1.1 of today's. Its mean and tail are reported, with no bar."""
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
os.environ.setdefault('RUN_WORK', str(Path.home() / '.cache' / 'pure-050'))  # on the card (/ is mmcblk0p2's ext4)
sys.path.insert(0, str(HERE.parent / '046-commit-replica'))
sys.path.insert(0, str(HERE.parent / '047-leaf-commit'))
import run as harness  # noqa: E402  (046)
import baseline  # noqa: E402  (047's workloads)

WORK = harness.WORK
if os.environ.get('SD_PURE_RUN'):  # another build's pure-run (sd.py merged sets it per run)
    harness.RUN = Path(os.environ['SD_PURE_RUN'])
REST = float(os.environ.get('SD_REST', '10'))  # seconds between runs, after a sync: the card's cache drains


def journal_commits():
    """The card's journal commits so far (jbd2's transactions on the device that holds WORK)."""
    device = os.stat(WORK).st_dev
    for info in Path('/proc/fs/jbd2').glob('*/info'):
        name = info.parent.name.rsplit('-', 1)[0]
        block = Path('/sys/class/block') / name / 'dev'
        if block.exists() and block.read_text().strip() == f'{os.major(device)}:{os.minor(device)}':
            return int(re.match(r'(\d+) transactions', info.read_text()).group(1))
    return None


def store_bytes(directory):
    return sum(p.stat().st_size for p in directory.rglob('*') if p.is_file())


def alone(module, commands, batch, store):
    """The owner alone, its syncs timed by the shim (nothing sent): seconds, the shim's counts, what it did."""
    shutil.rmtree(store, ignore_errors=True)
    stats = WORK / 'alone-stats.json'
    env = dict(harness.ENV, LD_PRELOAD=str(WORK / 'shim.so'), SHIM_STORE=str(store), SHIM_STATS=str(stats), SHIM_ALONE='1')
    extra = ['--batch', str(batch)] if batch > 1 else []
    began = time.perf_counter()
    done = subprocess.run([str(harness.RUN), str(WORK / f'{module}.pmod'), '--views', '--store', str(store), *extra],
                          input=''.join(commands), capture_output=True, text=True, env=env, timeout=3600)
    seconds = time.perf_counter() - began
    assert done.returncode == 0, done.stderr[-500:]
    said = sum(1 for line in done.stdout.splitlines() if line.startswith('{'))
    return seconds, json.loads(stats.read_text()), said


def shipped(keys, module, commands, batch):
    """046's owner, shipper and follower (both stores on the card): seconds, the shim's and the follower's counts."""
    r = harness.Run(keys, module=module, extra=('--batch', str(batch)) if batch > 1 else ())
    began = time.perf_counter()
    try:
        for c in commands:
            r.owner.stdin.write(c)
        r.owner.stdin.close()
    except BrokenPipeError:
        pass
    r.owner.wait()
    seconds = time.perf_counter() - began
    r.reader.join(timeout=30)
    r.shipper.communicate(timeout=60)
    follower = r.stop_follower()
    return seconds, json.loads((WORK / 'shim-stats.json').read_text()), r.done, follower


def measured(way, keys, module, commands, batch):
    os.sync()
    time.sleep(REST)
    before = journal_commits()
    if way == 'alone':
        seconds, shim, done = alone(module, commands, batch, WORK / 'owner-store')
        follower = None
    else:
        if way == 'shipped-data':
            harness.ENV['FOLLOWER_SYNC'] = 'data'
        try:
            seconds, shim, done, follower = shipped(keys, module, commands, batch)
        finally:
            harness.ENV.pop('FOLLOWER_SYNC', None)
    after = journal_commits()
    return {'seconds': round(seconds, 3), 'done': done, 'journal_commits': None if before is None else after - before,
            'shim': shim, 'follower': follower, 'owner_bytes': store_bytes(WORK / 'owner-store')}


def line(**fields):
    print(json.dumps(fields), flush=True)


def main():
    keys = harness.build()
    what = sys.argv[1]
    if what == 'entries':
        messages, rounds = int(sys.argv[2]), int(sys.argv[3])
        first = int(sys.argv[4]) if len(sys.argv) > 4 else 0
        commands = baseline.entries_commands(messages)[0]
        runs = [(way, batch) for batch in (1, 16, 256) for way in ('alone', 'shipped')]
        for k in range(first, first + rounds):
            for way, batch in runs[k % len(runs):] + runs[:k % len(runs)]:
                out = measured(way, keys, 'entries-price', commands, batch)
                line(workload='entries', round=k, way=way, batch=batch, messages=messages,
                     us_per_message=round(1e6 * out['seconds'] / messages, 1), **out)
    elif what == 'bank':
        accounts, transfers, rounds = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
        subprocess.run([str(harness.AUP), 'build', str(harness.PURE / 'benchmarks' / 'bank' / 'bank.aup'), '-o',
                        str(WORK / 'bank.pmod')], check=True)
        commands, setup = baseline.bank_commands(accounts, transfers)
        for k in range(rounds):
            for way in (('alone', 'shipped') if k % 2 == 0 else ('shipped', 'alone')):
                whole = measured(way, keys, 'bank', commands, 1)
                base = measured(way, keys, 'bank', commands[:setup], 1)
                line(workload='bank', round=k, way=way, batch=1, messages=transfers,
                     us_per_message=round(1e6 * (whole['seconds'] - base['seconds']) / transfers, 1), whole=whole, setup=base)
    elif what == 'follower':
        messages, rounds = int(sys.argv[2]), int(sys.argv[3])
        first = int(sys.argv[4]) if len(sys.argv) > 4 else 0
        commands = baseline.entries_commands(messages)[0]
        for k in range(first, first + rounds):
            for way in (('shipped', 'shipped-data') if k % 2 == 0 else ('shipped-data', 'shipped')):
                out = measured(way, keys, 'entries-price', commands, 1)
                line(workload='entries', round=k, way=way, batch=1, messages=messages,
                     us_per_message=round(1e6 * out['seconds'] / messages, 1), **out)
    elif what == 'merged':
        b0, messages, rounds = Path(sys.argv[2]).resolve(), int(sys.argv[3]), int(sys.argv[4])
        first = int(sys.argv[5]) if len(sys.argv) > 5 else 0
        today = harness.RUN
        commands = baseline.entries_commands(messages)[0]
        subprocess.run([str(harness.AUP), 'build', str(harness.PURE / 'benchmarks' / 'bank' / 'bank.aup'), '-o',
                        str(WORK / 'bank.pmod')], check=True)
        bank, setup = baseline.bank_commands(1000, messages)
        runs = [(build, batch) for batch in (1, 16, 256) for build in ('today', 'b0')]
        for k in range(first, first + rounds):
            for build, batch in runs[k % len(runs):] + runs[:k % len(runs)]:
                harness.RUN = b0 if build == 'b0' else today
                out = measured('alone', keys, 'entries-price', commands, batch)
                line(workload='entries', round=k, way='alone', build=build, batch=batch, messages=messages,
                     us_per_message=round(1e6 * out['seconds'] / messages, 1), **out)
            for build in (('today', 'b0') if k % 2 == 0 else ('b0', 'today')):
                harness.RUN = b0 if build == 'b0' else today
                out = measured('alone', keys, 'bank', bank, 1)
                line(workload='bank', round=k, way='alone', build=build, batch=1, messages=len(bank),
                     us_per_message=round(1e6 * out['seconds'] / len(bank), 1), **out)
        harness.RUN = today
    elif what == 'twice':
        messages = int(sys.argv[2])
        commands = baseline.entries_commands(messages)[0]
        stores = [WORK / 'twice-a', WORK / 'twice-b']
        for store in stores:
            alone('entries-price', commands, 1, store)
        line(workload='entries', run='twice', messages=messages, bytes=[store_bytes(s) for s in stores],
             differences=harness.stores_differ(*stores)[:10])


if __name__ == '__main__':
    main()
