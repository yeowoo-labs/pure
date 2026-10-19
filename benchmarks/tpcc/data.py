"""TPC-C's initial database (clause 4.3.3.1) for W warehouses, as rows of the
records in tpcc.aup (each composite key packed into one number, as there),
and the same rows for SQLite's tables with their own keys. Texts the
transactions only carry along are left out (see tpcc.aup). Money is in
cents and a rate in ten-thousandths, integers (i64s in tpcc.aup, INTEGER in
SQLite)."""
import random
import struct

ITEMS, CUSTOMERS, ORDERS, DELIVERED = 100000, 3000, 3000, 2100

# Each table's fields in record order (tpcc.aup), numbers but for the i64s in INTEGERS.
FIELDS = {
    'warehouses': ('id', 'tax', 'ytd'),
    'districts': ('id', 'tax', 'ytd', 'next_order', 'next_delivery'),
    'customers': ('id', 'discount', 'credit', 'balance', 'ytd_payment', 'payments', 'deliveries', 'last_order'),
    'items': ('id', 'price'),
    'stock': ('id', 'quantity', 'ytd', 'orders', 'remote'),
    'orders': ('id', 'customer', 'entry', 'carrier', 'lines', 'local'),
    'new_orders': ('id',),
    'lines': ('id', 'item', 'supply', 'delivery', 'quantity', 'amount'),
}
INTEGERS = {'tax', 'ytd', 'discount', 'balance', 'ytd_payment', 'price', 'amount'}


def district_key(w, d):
    return w * 10 + d


def customer_key(w, d, c):
    return district_key(w, d) * 3000 + c


def order_key(w, d, o):
    return district_key(w, d) * 10000000 + o


def generate(warehouses, seed=1, now=1.7e12):
    """The tables as lists of tuples in FIELDS order."""
    rng = random.Random(seed)
    tables = {name: [] for name in FIELDS}
    tables['items'] = [(i, rng.randint(100, 10000)) for i in range(1, ITEMS + 1)]
    for w in range(1, warehouses + 1):
        tables['warehouses'].append((w, rng.randint(0, 2000), 30000000))
        tables['stock'].extend((w * 100000 + i, rng.randint(10, 100), 0, 0, 0) for i in range(1, ITEMS + 1))
        for d in range(1, 11):
            tables['districts'].append((district_key(w, d), rng.randint(0, 2000), 3000000, ORDERS + 1, DELIVERED + 1))
            owners = list(range(1, CUSTOMERS + 1))
            rng.shuffle(owners)
            last = {}
            for o in range(1, ORDERS + 1):
                key, count, delivered = order_key(w, d, o), rng.randint(5, 15), o <= DELIVERED
                tables['orders'].append((key, customer_key(w, d, owners[o - 1]), now,
                                         rng.randint(1, 10) if delivered else 0, count, 1))
                last[owners[o - 1]] = key
                if not delivered:
                    tables['new_orders'].append((key,))
                for n in range(1, count + 1):
                    tables['lines'].append((key * 16 + n, rng.randint(1, ITEMS), w, now if delivered else 0, 5,
                                            0 if delivered else rng.randint(1, 999999)))
            for c in range(1, CUSTOMERS + 1):
                tables['customers'].append((customer_key(w, d, c), rng.randint(0, 5000),
                                            0 if rng.random() < 0.1 else 1, -1000, 1000, 1, 0, last[c]))
    return tables


def packed(table, rows):
    """A table's rows in a list's stored form (each number's binary64, each i64's two's complement, little-endian)."""
    row = '<' + ''.join('q' if field in INTEGERS else 'd' for field in FIELDS[table])
    return b''.join(struct.pack(row, *r) for r in rows)
