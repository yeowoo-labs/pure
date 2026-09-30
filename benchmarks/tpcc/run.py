#!/usr/bin/env python3
"""TPC-C on Pure and on SQLite (docs/research/092-tpcc.md).

tpcc.aup is TPC-C's tables and five transactions as an aup app; data.py makes
the initial database of clause 4.3.3.1 for --warehouses warehouses. The same
rows go into pure-serve (in bulk, as binary rows) and into SQLite (the
specification's tables with their composite primary keys, WITHOUT ROWID,
WAL), both kept in --directory. Then:

1. The same transactions on both: one terminal, --check transactions drawn
   from one seed by tpcc-load (over HTTP) and by sqlite-tpcc (in process),
   and every row of every table compared (times aside), Pure's read through
   queries registered for the purpose.
2. Throughput: --seconds of the whole mix, Pure's from each count of
   --terminals connections (tpcc-load on core 3, the server on cores 0-2),
   SQLite's from each count of --sqlite-threads threads, each with its own
   connection, on cores 0-2, synchronous --sync (FULL syncs each commit
   before it is answered, as Pure does). With --sqlite-group, SQLite again
   from each count of terminals handing their transactions to one writer
   that commits those waiting together (as pure-serve does), on a copy of
   the database checked the same way: after the same transactions, every
   row equal to the other SQLite's. --sqlite-pragmas are run on each of
   SQLite's connections (tuned's unless given), its tables made with pages
   of --sqlite-page-size bytes. Each run reports its CPU a transaction
   (Pure's server's, and the share of a core its owner thread took), its
   memory (Pure's server's resident, SQLite's peak), and the bytes a
   transaction had written: by the process, and to the device holding
   --directory.
3. The consistency conditions of clause 3.3.2 on both after each run, each side read from one snapshot.
4. At the end, the bytes Pure's states held after its runs (it is stopped before SQLite's), and the files of both.

Prints one JSON line a step.
"""
import argparse
import http.client
import json
import os
import select
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import data  # noqa: E402

SCHEMA = '''
CREATE TABLE warehouse (w_id INTEGER PRIMARY KEY, w_tax REAL, w_ytd REAL);
CREATE TABLE district (d_w_id INTEGER, d_id INTEGER, d_tax REAL, d_ytd REAL, d_next_o_id INTEGER, d_next_delivery INTEGER,
  PRIMARY KEY (d_w_id, d_id)) WITHOUT ROWID;
CREATE TABLE customer (c_w_id INTEGER, c_d_id INTEGER, c_id INTEGER, c_discount REAL, c_credit INTEGER, c_balance REAL,
  c_ytd_payment REAL, c_payment_cnt INTEGER, c_delivery_cnt INTEGER, c_last_order INTEGER,
  PRIMARY KEY (c_w_id, c_d_id, c_id)) WITHOUT ROWID;
CREATE TABLE item (i_id INTEGER PRIMARY KEY, i_price REAL);
CREATE TABLE stock (s_w_id INTEGER, s_i_id INTEGER, s_quantity INTEGER, s_ytd INTEGER, s_order_cnt INTEGER,
  s_remote_cnt INTEGER, PRIMARY KEY (s_w_id, s_i_id)) WITHOUT ROWID;
CREATE TABLE orders (o_w_id INTEGER, o_d_id INTEGER, o_id INTEGER, o_c_id INTEGER, o_entry_d REAL, o_carrier_id INTEGER,
  o_ol_cnt INTEGER, o_all_local INTEGER, PRIMARY KEY (o_w_id, o_d_id, o_id)) WITHOUT ROWID;
CREATE TABLE new_order (no_w_id INTEGER, no_d_id INTEGER, no_o_id INTEGER,
  PRIMARY KEY (no_w_id, no_d_id, no_o_id)) WITHOUT ROWID;
CREATE TABLE order_line (ol_w_id INTEGER, ol_d_id INTEGER, ol_o_id INTEGER, ol_number INTEGER, ol_i_id INTEGER,
  ol_supply_w_id INTEGER, ol_delivery_d REAL, ol_quantity INTEGER, ol_amount REAL,
  PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number)) WITHOUT ROWID;
CREATE TABLE history (h_c_w_id INTEGER, h_c_d_id INTEGER, h_c_id INTEGER, h_w_id INTEGER, h_d_id INTEGER, h_date REAL,
  h_amount REAL);
'''

ORDERS = 10000000  # an order's key is its district's times this, plus its number


def tuned(page_size):
    """SQLite as a careful engineer runs it (docs/research/106): the database mapped, a page cache large enough for
    it, a savepoint's journal in memory, a checkpoint each 40 MB of WAL (its default is each 1,000 pages)."""
    return ('PRAGMA mmap_size=1073741824; PRAGMA cache_size=-65536; PRAGMA temp_store=MEMORY; '
            f'PRAGMA wal_autocheckpoint={40960000 // page_size};')


def district_of(key):
    """A district key's warehouse and district number."""
    return (key - 1) // 10, (key - 1) % 10 + 1


def sqlite_rows(tables):
    """data.generate's rows as the rows of SQLite's tables."""
    def customer(key):
        return (*district_of((key - 1) // 3000), (key - 1) % 3000 + 1)

    def order(key):
        return (*district_of(key // ORDERS), key % ORDERS)

    yield 'warehouse', tables['warehouses']
    yield 'district', [(*district_of(k), tax, ytd, next_order, next_delivery)
                       for k, tax, ytd, next_order, next_delivery in tables['districts']]
    yield 'customer', [(*customer(k), discount, credit, balance, ytd, payments, deliveries, last % ORDERS)
                       for k, discount, credit, balance, ytd, payments, deliveries, last in tables['customers']]
    yield 'item', tables['items']
    yield 'stock', [((k - 1) // 100000, (k - 1) % 100000 + 1, *rest) for k, *rest in tables['stock']]
    yield 'orders', [(*order(k), (c - 1) % 3000 + 1, *rest) for k, c, *rest in tables['orders']]
    yield 'new_order', [order(k) for k, in tables['new_orders']]
    yield 'order_line', [(*order(k // 16), k % 16, *rest) for k, *rest in tables['lines']]


def load_sqlite(path, tables, page_size):
    db = sqlite3.connect(str(path))
    db.executescript(f'PRAGMA page_size={page_size}; PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;' + SCHEMA)
    with db:
        for table, rows in sqlite_rows(tables):
            db.executemany(f'INSERT INTO {table} VALUES ({", ".join("?" * len(rows[0]))})', rows)
    db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
    db.close()


class Pure:
    """pure-serve on cores 0-2 with the app's store in directory."""

    def __init__(self, serve, module, store, threads):
        self.process = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--module', str(module), '--store', str(store),
                                         '--port', '0', '--threads', str(threads), '--timeout-ms', '600000'],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        select.select([self.process.stdout], [], [], 30)
        self.port = json.loads(self.process.stdout.readline())['port']
        self.connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=600)

    def call(self, method, path, body=None, kind='application/json'):
        self.connection.request(method, path, body, {'Content-Type': kind} if body is not None else {})
        answer = self.connection.getresponse()
        return answer.status, answer.read()

    def view(self, name):
        status, body = self.call('GET', '/views/' + name)
        assert status == 200, (name, status, body[:200])
        return json.loads(body)

    def load(self, tables):
        for name, rows in tables.items():
            if name == 'history':
                continue
            chunk = max(1, (1400000 // 8) // len(data.FIELDS[name]) - 1)
            for at in range(0, len(rows), chunk):
                status, body = self.call('POST', f'/inputs/load_{name}', data.packed(rows[at:at + chunk]),
                                         'application/octet-stream')
                assert status == 200, (name, status, body[:200])

    def cpu_seconds(self):
        fields = Path(f'/proc/{self.process.pid}/stat').read_text().rsplit(')', 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')

    def resident_mib(self):
        for line in Path(f'/proc/{self.process.pid}/status').read_text().splitlines():
            if line.startswith('VmRSS:'):
                return round(int(line.split()[1]) / 1024, 1)
        return None

    def threads_cpu(self):
        """The CPU seconds of the server's threads by name (its owner is pure-owner)."""
        seconds = {}
        for task in Path(f'/proc/{self.process.pid}/task').iterdir():
            try:
                name = (task / 'comm').read_text().strip()
                fields = (task / 'stat').read_text().rsplit(')', 1)[1].split()
            except OSError:  # gone meanwhile
                continue
            seconds[name] = seconds.get(name, 0) + (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')
        return seconds

    def written(self):
        for line in Path(f'/proc/{self.process.pid}/io').read_text().splitlines():
            if line.startswith('write_bytes:'):
                return int(line.split()[1])
        return 0

    def stop(self):
        """The counts the server prints as it stops (state_bytes among them)."""
        self.connection.close()
        self.process.terminate()
        lines = self.process.communicate(timeout=120)[1].decode().strip().splitlines()
        return json.loads(lines[-1]) if lines and lines[-1].startswith('{') else {}


# Each table's rows, times aside, as Pure's queries and SQLite's SELECTs name them (keys packed as tpcc.aup packs them).
DUMPS = {
    'warehouses': ('each w in warehouses {\n  id = w.id\n  ytd = w.ytd\n}',
                   'SELECT w_id, w_ytd FROM warehouse'),
    'districts': ('each d in districts {\n  id = d.id\n  ytd = d.ytd\n  next_order = d.next_order\n'
                  '  next_delivery = d.next_delivery\n}',
                  'SELECT d_w_id * 10 + d_id, d_ytd, d_next_o_id, d_next_delivery FROM district'),
    'customers': ('each c in customers {\n  id = c.id\n  balance = c.balance\n  ytd_payment = c.ytd_payment\n'
                  '  payments = c.payments\n  deliveries = c.deliveries\n  last_order = c.last_order\n}',
                  'SELECT (c_w_id * 10 + c_d_id) * 3000 + c_id, c_balance, c_ytd_payment, c_payment_cnt, c_delivery_cnt, '
                  '(c_w_id * 10 + c_d_id) * 10000000 + c_last_order FROM customer'),
    'stock': ('each s in stock {\n  id = s.id\n  quantity = s.quantity\n  ytd = s.ytd\n  orders = s.orders\n'
              '  remote = s.remote\n}',
              'SELECT s_w_id * 100000 + s_i_id, s_quantity, s_ytd, s_order_cnt, s_remote_cnt FROM stock'),
    'orders': ('each o in orders {\n  id = o.id\n  customer = o.customer\n  carrier = o.carrier\n  lines = o.lines\n'
               '  local = o.local\n}',
               'SELECT (o_w_id * 10 + o_d_id) * 10000000 + o_id, (o_w_id * 10 + o_d_id) * 3000 + o_c_id, o_carrier_id, '
               'o_ol_cnt, o_all_local FROM orders'),
    'new_orders': ('each n in new_orders {\n  id = n.id\n}',
                   'SELECT (no_w_id * 10 + no_d_id) * 10000000 + no_o_id FROM new_order'),
    'lines': ('each l in lines {\n  id = l.id\n  item = l.item\n  supply = l.supply\n  quantity = l.quantity\n'
              '  amount = l.amount\n  delivery = l.delivery\n}',
              'SELECT ((ol_w_id * 10 + ol_d_id) * 10000000 + ol_o_id) * 16 + ol_number, ol_i_id, ol_supply_w_id, '
              'ol_quantity, ol_amount, ol_delivery_d FROM order_line'),
    'history': ('each h in history {\n  customer = h.customer\n  district = h.district\n  amount = h.amount\n}',
                'SELECT (h_c_w_id * 10 + h_c_d_id) * 3000 + h_c_id, h_w_id * 10 + h_d_id, h_amount FROM history'),
}

# Clause 3.3.2.1-4, a district a row, from one snapshot (one query, one SELECT): its next order, its newest
# order, its oldest and newest new orders and how many (orders by their numbers in the district), the lines its
# orders count and the lines it has, and the warehouses' and districts' year-to-date totals.
DISTRICTS = ('each d in districts {\n  id = d.id\n  next_order = d.next_order\n'
             '  newest = max(o.id for o in orders[d.id * 10000000 .. d.id * 10000000 + 10000000]) - d.id * 10000000\n'
             '  newest_new = max(n.id for n in new_orders[d.id * 10000000 .. d.id * 10000000 + 10000000])'
             ' - d.id * 10000000\n'
             '  oldest_new = min(n.id for n in new_orders[d.id * 10000000 .. d.id * 10000000 + 10000000])'
             ' - d.id * 10000000\n'
             '  new_orders = count(n in new_orders[d.id * 10000000 .. d.id * 10000000 + 10000000])\n'
             '  ordered = sum(o.lines for o in orders[d.id * 10000000 .. d.id * 10000000 + 10000000])\n'
             '  lines = count(l in lines[d.id * 160000000 .. d.id * 160000000 + 160000000])\n'
             '  warehouse_ytd = sum(w.ytd for w in warehouses)\n'
             '  district_ytd = sum(x.ytd for x in districts)\n}',
             'SELECT d_w_id * 10 + d_id, d_next_o_id, '
             '(SELECT max(o_id) FROM orders WHERE o_w_id = d_w_id AND o_d_id = d_id), '
             '(SELECT max(no_o_id) FROM new_order WHERE no_w_id = d_w_id AND no_d_id = d_id), '
             '(SELECT min(no_o_id) FROM new_order WHERE no_w_id = d_w_id AND no_d_id = d_id), '
             '(SELECT count(*) FROM new_order WHERE no_w_id = d_w_id AND no_d_id = d_id), '
             '(SELECT sum(o_ol_cnt) FROM orders WHERE o_w_id = d_w_id AND o_d_id = d_id), '
             '(SELECT count(*) FROM order_line WHERE ol_w_id = d_w_id AND ol_d_id = d_id), '
             '(SELECT sum(w_ytd) FROM warehouse), (SELECT sum(d_ytd) FROM district) FROM district')


def register(pure, aup, work, name, view):
    source = work / f'{name}.aup'
    source.write_text(f'view {name} = {view}\n')
    subprocess.run([str(aup), 'query', str(HERE / 'tpcc.aup'), str(source), '-o', str(work / f'{name}.pmod')], check=True)
    status, body = pure.call('PUT', f'/queries/{name}', (work / f'{name}.pmod').read_bytes(), 'application/octet-stream')
    assert status == 200, (name, status, body[:200])


def asked(pure, name):
    status, body = pure.call('GET', f'/queries/{name}')
    assert status == 200, (name, status, body[:200])
    return [tuple(row.values()) for row in json.loads(body)['items']]


def same(a, b):
    return a == b or abs(a - b) <= 1e-9 * max(1.0, abs(a), abs(b))


def compare(table, pure_rows, sqlite_rows_):
    """Rows that differ between the two (a table's rows in key order; history's sorted)."""
    if table in ('lines',):  # a line's delivery time differs; whether it has one must not
        pure_rows = [(*r[:-1], r[-1] > 0) for r in pure_rows]
        sqlite_rows_ = [(*r[:-1], r[-1] > 0) for r in sqlite_rows_]
    pure_rows, sqlite_rows_ = sorted(pure_rows), sorted(sqlite_rows_)
    if len(pure_rows) != len(sqlite_rows_):
        return [('count', len(pure_rows), len(sqlite_rows_))]
    return [(p, s) for p, s in zip(pure_rows, sqlite_rows_) if not all(same(x, y) for x, y in zip(p, s))]


def device_written(directory):
    """Bytes written so far to the block device holding directory (None on tmpfs)."""
    device = os.stat(directory).st_dev
    stat = Path(f'/sys/dev/block/{os.major(device)}:{os.minor(device)}/stat')
    return int(stat.read_text().split()[6]) * 512 if stat.exists() else None


def per(value, count, scale=1):
    return None if value is None else round(value / max(count, 1) * scale, 1)


def files_mib(place, prefix):
    """The files named prefix... in place, and those in directories so named (the store is one)."""
    named = list(place.glob(prefix + '*'))
    files = [f for p in named for f in ([p] if p.is_file() else p.rglob('*')) if f.is_file()]
    return round(sum(f.stat().st_size for f in files) / 2**20, 1)


def consistency(pure, db, sqlite_only=False):
    """Clause 3.3.2.1-4 on both (or SQLite's alone), each read from one snapshot: the conditions failed (none when
    consistent)."""
    failures = []
    sides = [] if sqlite_only else [('pure', asked(pure, 'district_check'))]
    for system, districts in sides + [('sqlite', db.execute(DISTRICTS[1]).fetchall())]:
        for key, next_order, newest, newest_new, oldest_new, new_orders, ordered, lines, ytd, district_ytd in districts:
            if not same(ytd, district_ytd):
                failures.append((system, '3.3.2.1', ytd, district_ytd))
            if not next_order - 1 == newest == newest_new:
                failures.append((system, '3.3.2.2', key, next_order, newest, newest_new))
            if newest_new - oldest_new + 1 != new_orders:
                failures.append((system, '3.3.2.3', key, oldest_new, newest_new, new_orders))
            if ordered != lines:
                failures.append((system, '3.3.2.4', key, ordered, lines))
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aup', type=Path, default=ROOT.parent / 'aup' / 'build' / 'aup')
    parser.add_argument('--serve', type=Path, default=ROOT / 'build' / 'pure-serve')
    parser.add_argument('--directory', type=Path, default=Path('/dev/shm'), help='where the store and database live')
    parser.add_argument('--warehouses', type=int, default=1)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--check', type=int, default=20000, help='transactions run on both and compared (0: none)')
    parser.add_argument('--seconds', type=float, default=20)
    parser.add_argument('--terminals', default='16,64', help="Pure's connections, a run each")
    parser.add_argument('--threads', default='3', help="pure-serve's --threads")
    parser.add_argument('--sqlite-threads', default='1,4,16', help="SQLite's threads, a run each")
    parser.add_argument('--sync', default='full', choices=('full', 'normal', 'off'))
    parser.add_argument('--sqlite-group', default='', help="SQLite's terminals committing together, a run each")
    parser.add_argument('--sqlite-page-size', type=int, default=4096, help="SQLite's page size (its default)")
    parser.add_argument('--sqlite-pragmas', help="run on each of SQLite's connections (SQLITE_PRAGMAS; tuned's unless "
                        "given, '' for SQLite's defaults, as research 092 ran it)")
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix='pure-tpcc-'))
    place = Path(tempfile.mkdtemp(prefix='pure-tpcc-', dir=args.directory))
    pure = None
    pragmas = tuned(args.sqlite_page_size) if args.sqlite_pragmas is None else args.sqlite_pragmas
    sqlite_env = {**os.environ, 'SQLITE_PRAGMAS': pragmas}
    kinds = ('new_order', 'payment', 'order_status', 'delivery', 'stock_level')
    try:
        module = work / 'tpcc.pmod'
        subprocess.run([str(args.aup), 'build', str(HERE / 'tpcc.aup'), '-o', str(module)], check=True)
        subprocess.run(['cc', '-O2', '-o', str(work / 'tpcc-load'), str(HERE / 'tpcc-load.c')], check=True)
        subprocess.run(['cc', '-O2', '-o', str(work / 'sqlite-tpcc'), str(HERE / 'sqlite-tpcc.c'), '-lsqlite3', '-lpthread'],
                       check=True)
        began = time.perf_counter()
        tables = data.generate(args.warehouses, args.seed)
        rows = sum(len(t) for t in tables.values())
        generated = time.perf_counter() - began
        pure = Pure(args.serve, module, place / 'store', args.threads)
        began = time.perf_counter()
        pure.load(tables)
        pure_load = time.perf_counter() - began
        database = place / 'tpcc.db'
        began = time.perf_counter()
        load_sqlite(database, tables, args.sqlite_page_size)
        sqlite_load = time.perf_counter() - began
        grouped = place / 'tpcc-group.db'
        if args.sqlite_group:
            shutil.copyfile(database, grouped)
        counts = {'order_count': len(tables['orders']), 'new_order_count': len(tables['new_orders']),
                  'line_count': len(tables['lines'])}
        assert {v: pure.view(v) for v in counts} == counts
        print(json.dumps({'step': 'load', 'directory': str(args.directory), 'warehouses': args.warehouses, 'rows': rows,
                          'generate_s': round(generated, 2), 'pure_load_s': round(pure_load, 2),
                          'sqlite_load_s': round(sqlite_load, 2), 'sqlite_page_size': args.sqlite_page_size,
                          'pure_resident_mib': pure.resident_mib(),
                          'sqlite_file_mib': round(database.stat().st_size / 2**20, 1)}), flush=True)
        db = sqlite3.connect(str(database))
        register(pure, args.aup, work, 'district_check', DISTRICTS[0])
        if args.check:
            pure_run = json.loads(subprocess.run(
                [str(work / 'tpcc-load'), str(pure.port), '1', '3600', str(args.warehouses), str(args.seed),
                 str(args.check)], capture_output=True, text=True, check=True).stdout)
            sqlite_run = json.loads(subprocess.run(
                [str(work / 'sqlite-tpcc'), str(database), str(args.warehouses), '1', '3600', args.sync, str(args.seed),
                 str(args.check)], capture_output=True, text=True, check=True, env=sqlite_env).stdout)
            differing, group_differing = {}, {}
            group_db = None
            if args.sqlite_group:
                subprocess.run([str(work / 'sqlite-tpcc'), str(grouped), str(args.warehouses), '1', '3600', args.sync,
                                str(args.seed), str(args.check), 'group'], capture_output=True, text=True, check=True,
                               env=sqlite_env)
                group_db = sqlite3.connect(str(grouped))
            for table, (view, sql) in DUMPS.items():
                register(pure, args.aup, work, f'dump_{table}', view)
                expected = db.execute(sql).fetchall()
                found = compare(table, asked(pure, f'dump_{table}'), expected)
                if found:
                    differing[table] = [len(found), found[:3]]
                found = compare(table, group_db.execute(sql).fetchall(), expected) if group_db else []
                if found:
                    group_differing[table] = [len(found), found[:3]]
            if group_db is not None:
                group_db.close()
            print(json.dumps({'step': 'same transactions', 'transactions': args.check,
                              'pure': {k: pure_run[k]['done'] for k in kinds},
                              'sqlite': {k: sqlite_run[k]['done'] for k in kinds},
                              'undone_new_orders': [pure_run['undone_new_orders'], sqlite_run['undone_new_orders']],
                              'failed': [sum(pure_run[k]['failed'] for k in kinds),
                                         sum(sqlite_run[k]['failed'] for k in kinds)],
                              'differing': differing, 'grouped_sqlite_differing': group_differing,
                              'consistency_failures': consistency(pure, db)}), flush=True)
        for terminals in (int(t) for t in args.terminals.split(',') if t):
            os.sync()  # what loading and copying left is not written during a run
            used, threads_used, written, device = pure.cpu_seconds(), pure.threads_cpu(), pure.written(), \
                device_written(place)
            began = time.perf_counter()
            result = json.loads(subprocess.run(
                ['taskset', '-c', '3', str(work / 'tpcc-load'), str(pure.port), str(terminals), str(args.seconds),
                 str(args.warehouses), str(args.seed + terminals)], capture_output=True, text=True, check=True).stdout)
            took = time.perf_counter() - began
            used = pure.cpu_seconds() - used
            share = {name: round((seconds - threads_used.get(name, 0)) / took, 2)
                     for name, seconds in pure.threads_cpu().items() if name in ('pure-owner', 'pure-checkpoint')}
            done = sum(result[k]['done'] for k in kinds)
            device = None if device is None else device_written(place) - device
            print(json.dumps({'step': 'run', **result, 'server_threads': args.threads,
                              'cpu_us_per_transaction': per(used, done, 1e6), 'core_shares': share,
                              'resident_mib': pure.resident_mib(),
                              'written_bytes_per_transaction': per(pure.written() - written, done),
                              'device_bytes_per_transaction': per(device, done),
                              'consistency_failures': consistency(pure, db)}), flush=True)
        resident = pure.resident_mib()
        counts, pure = pure.stop(), None  # its checkpoints are not written during SQLite's runs
        runs = [(database, threads, []) for threads in (int(t) for t in args.sqlite_threads.split(',') if t)]
        runs += [(grouped, terminals, ['group']) for terminals in (int(t) for t in args.sqlite_group.split(',') if t)]
        for path, threads, mode in runs:
            os.sync()
            device = device_written(place)
            result = json.loads(subprocess.run(
                ['taskset', '-c', '0-2', str(work / 'sqlite-tpcc'), str(path), str(args.warehouses), str(threads),
                 str(args.seconds), args.sync, str(args.seed + threads), *mode], capture_output=True, text=True,
                check=True, env=sqlite_env).stdout)
            device = None if device is None else device_written(place) - device
            done = sum(result[k]['done'] for k in kinds)
            checked = sqlite3.connect(str(path))
            print(json.dumps({'step': 'run', **result, 'cpu_us_per_transaction': per(result['cpu_seconds'], done, 1e6),
                              'peak_resident_mib': round(result['peak_resident_kib'] / 1024, 1),
                              'written_bytes_per_transaction': per(result['written_bytes'], done),
                              'device_bytes_per_transaction': per(device, done),
                              'consistency_failures': consistency(pure, checked, sqlite_only=True)}), flush=True)
            checked.close()
        db.close()
        print(json.dumps({'step': 'end', 'pure_resident_mib': resident,
                          'pure_state_mib': round(counts.get('state_bytes', 0) / 2**20, 1),
                          'pure_store_mib': files_mib(place, 'store'), 'sqlite_files_mib': files_mib(place, 'tpcc.db'),
                          'grouped_sqlite_files_mib': files_mib(place, 'tpcc-group.db'),
                          'pure_counts': {k: counts.get(k) for k in ('bursts', 'burst_events', 'checkpoints')}}),
              flush=True)
    finally:
        if pure:
            pure.stop()
        shutil.rmtree(place, ignore_errors=True)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
