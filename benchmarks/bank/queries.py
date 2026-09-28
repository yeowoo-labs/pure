#!/usr/bin/env python3
"""Queries over a bank of 100,000 accounts: Pure's registered queries beside
the same SQL on SQLite (docs/research/091-queries.md).

The bank (bank.aup) gets --branches branches, ten tellers each, --accounts
accounts spread over the branches and --transfers transfers, posted over HTTP;
SQLite (in RAM, the pgbench tables) gets the same rows. Then each query is
asked --times times on both, Pure's through GET /queries/NAME (registered
once, shown from the last save's snapshot, HTTP and JSON included), SQLite's
through its C library from Python, on pgbench's tables as they are and again
with the indexes a DBA would add for these queries (accounts by balance, and
by branch with the balance) after ANALYZE. Pure is given no index: it plans
from the query alone. The answers are checked to agree. Prints one JSON line
per query: the median and fastest time of each.
"""
import argparse
import http.client
import json
import random
import select
import shutil
import sqlite3
import statistics
import subprocess
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

QUERIES = {
    # per branch: its total and how many accounts (a GROUP BY, as a fold per row)
    'per_branch': ('view per_branch = each b in branches {\n  bid = b.bid\n'
                   '  total = sum(a.balance for a in accounts where a.bid == b.bid)\n'
                   '  accounts = count(a in accounts where a.bid == b.bid)\n}\n',
                   'SELECT bid, sum(abalance), count(*) FROM pgbench_accounts GROUP BY bid ORDER BY bid'),
    # accounts in credit (a filter over every row)
    'in_credit': ('view in_credit = count(a in accounts where a.balance > 0)\n',
                  'SELECT count(*) FROM pgbench_accounts WHERE abalance > 0'),
    # large transfers with their account's and teller's balance (a three-way join)
    'large': ('view large = each h in history where h.delta > 4900 {\n  aid = h.aid\n  delta = h.delta\n'
              '  account = accounts[h.aid].balance\n  teller = tellers[h.tid].balance\n}\n',
              'SELECT h.aid, h.delta, a.abalance, t.tbalance FROM pgbench_history h '
              'JOIN pgbench_accounts a ON a.aid = h.aid JOIN pgbench_tellers t ON t.tid = h.tid '
              'WHERE h.delta > 4900 ORDER BY h.rowid'),
    # the richest accounts and how many are richer (a correlated self-join)
    'ranks': ('view ranks = each a in accounts where a.balance > 4900 {\n  aid = a.aid\n'
              '  richer = count(b in accounts where b.balance > a.balance)\n}\n',
              'SELECT a.aid, (SELECT count(*) FROM pgbench_accounts b WHERE b.abalance > a.abalance) '
              'FROM pgbench_accounts a WHERE a.abalance > 4900 ORDER BY a.aid'),
}


def post_all(port, path, bodies, threads=8):
    failed = []

    def worker(part):
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=60)
        for body in part:
            connection.request('POST', path, body, {'Content-Type': 'application/json'})
            answer = connection.getresponse()
            answer.read()
            if answer.status != 200:
                failed.append(answer.status)
        connection.close()

    workers = [threading.Thread(target=worker, args=(bodies[i::threads],)) for i in range(threads)]
    for w in workers:
        w.start()
    for w in workers:
        w.join()
    assert not failed, failed[:3]


def rows_of(name, data):
    """Pure's answer as SQLite's tuples."""
    if name == 'in_credit':
        return [(int(data),)]
    items = json.loads(data)['items']
    return [tuple(int(v) for v in row.values()) for row in items]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aup', type=Path, default=ROOT.parent / 'aup' / 'build' / 'aup')
    parser.add_argument('--serve', type=Path, default=ROOT / 'build' / 'pure-serve')
    parser.add_argument('--accounts', type=int, default=100000)
    parser.add_argument('--branches', type=int, default=10)
    parser.add_argument('--transfers', type=int, default=20000)
    parser.add_argument('--times', type=int, default=15)
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix='pure-queries-', dir='/dev/shm'))
    process = None
    try:
        module = work / 'bank.pmod'
        subprocess.run([str(args.aup), 'build', str(HERE / 'bank.aup'), '-o', str(module)], check=True)
        compiled = {}
        for name, (view, _) in QUERIES.items():
            source = work / f'{name}.aup'
            source.write_text(view)
            subprocess.run([str(args.aup), 'query', str(HERE / 'bank.aup'), str(source), '-o', str(work / f'{name}.pmod')],
                           check=True)
            compiled[name] = (work / f'{name}.pmod').read_bytes()
        process = subprocess.Popen(['taskset', '-c', '0-2', str(args.serve), '--module', str(module), '--store',
                                    str(work / 'store'), '--port', '0', '--timeout-ms', '600000'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        select.select([process.stdout], [], [], 10)
        port = json.loads(process.stdout.readline())['port']
        rng = random.Random(9)
        tellers = 10 * args.branches
        transfers = [(rng.randint(1, args.accounts), rng.randint(1, tellers), rng.randint(1, args.branches),
                      rng.randint(-5000, 5000)) for _ in range(args.transfers)]
        post_all(port, '/inputs/branch', [json.dumps({'bid': b}).encode() for b in range(1, args.branches + 1)], 1)
        post_all(port, '/inputs/teller', [json.dumps({'tid': t, 'bid': (t - 1) // 10 + 1}).encode()
                                          for t in range(1, tellers + 1)], 1)
        post_all(port, '/inputs/account', [json.dumps({'aid': a, 'bid': (a - 1) % args.branches + 1}).encode()
                                           for a in range(1, args.accounts + 1)])
        post_all(port, '/inputs/transfer', [json.dumps({'aid': a, 'tid': t, 'bid': b, 'delta': d}).encode()
                                            for a, t, b, d in transfers], 1)  # one client: history in this order
        db = sqlite3.connect(str(work / 'bank.db'))
        db.executescript('PRAGMA journal_mode=WAL;'
                         'CREATE TABLE pgbench_accounts (aid INTEGER PRIMARY KEY, bid INTEGER, abalance INTEGER, filler CHAR(84));'
                         'CREATE TABLE pgbench_tellers (tid INTEGER PRIMARY KEY, bid INTEGER, tbalance INTEGER, filler CHAR(84));'
                         'CREATE TABLE pgbench_branches (bid INTEGER PRIMARY KEY, bbalance INTEGER, filler CHAR(88));'
                         'CREATE TABLE pgbench_history (tid INTEGER, bid INTEGER, aid INTEGER, delta INTEGER, mtime TIMESTAMP,'
                         ' filler CHAR(22));')
        db.executemany('INSERT INTO pgbench_branches VALUES (?, 0, \'\')', [(b,) for b in range(1, args.branches + 1)])
        db.executemany('INSERT INTO pgbench_tellers VALUES (?, ?, 0, \'\')',
                       [(t, (t - 1) // 10 + 1) for t in range(1, tellers + 1)])
        db.executemany('INSERT INTO pgbench_accounts VALUES (?, ?, 0, \'\')',
                       [(a, (a - 1) % args.branches + 1) for a in range(1, args.accounts + 1)])
        for a, t, b, d in transfers:
            db.execute('UPDATE pgbench_accounts SET abalance = abalance + ? WHERE aid = ?', (d, a))
            db.execute('UPDATE pgbench_tellers SET tbalance = tbalance + ? WHERE tid = ?', (d, t))
            db.execute('UPDATE pgbench_branches SET bbalance = bbalance + ? WHERE bid = ?', (d, b))
            db.execute('INSERT INTO pgbench_history (tid, bid, aid, delta) VALUES (?, ?, ?, ?)', (t, b, a, d))
        db.commit()
        client = http.client.HTTPConnection('127.0.0.1', port, timeout=120)
        for name, bytes_ in compiled.items():
            client.request('PUT', f'/queries/{name}', bytes_)
            answer = client.getresponse()
            answer.read()
            assert answer.status == 200, (name, answer.status)
        def timed(ask):
            times, answer = [], None
            for _ in range(args.times):
                began = time.perf_counter()
                answer = ask()
                times.append(time.perf_counter() - began)
            return answer, times

        def pure(name):
            client.request('GET', f'/queries/{name}')
            answer = client.getresponse()
            data = answer.read()
            assert answer.status == 200, (name, answer.status)
            return data

        results = {}
        for name, (_, sql) in QUERIES.items():
            data, pure_times = timed(lambda: pure(name))
            rows, sqlite_times = timed(lambda: db.execute(sql).fetchall())
            assert rows_of(name, data) == [tuple(int(v) for v in row) for row in rows], name
            results[name] = (len(rows), pure_times, sqlite_times)
        db.executescript('CREATE INDEX accounts_balance ON pgbench_accounts (abalance);'
                         'CREATE INDEX accounts_branch ON pgbench_accounts (bid, abalance); ANALYZE;')
        for name, (_, sql) in QUERIES.items():
            rows, indexed_times = timed(lambda: db.execute(sql).fetchall())
            count, pure_times, sqlite_times = results[name]
            assert len(rows) == count, name
            print(json.dumps({'query': name, 'rows': count, 'accounts': args.accounts,
                              'pure_median_ms': round(statistics.median(pure_times) * 1e3, 2),
                              'pure_fastest_ms': round(min(pure_times) * 1e3, 2),
                              'sqlite_median_ms': round(statistics.median(sqlite_times) * 1e3, 2),
                              'sqlite_fastest_ms': round(min(sqlite_times) * 1e3, 2),
                              'sqlite_indexed_median_ms': round(statistics.median(indexed_times) * 1e3, 2),
                              'sqlite_indexed_fastest_ms': round(min(indexed_times) * 1e3, 2)}), flush=True)
    finally:
        if process:
            process.terminate()
            process.communicate(timeout=60)
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
