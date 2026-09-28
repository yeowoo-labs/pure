#define _GNU_SOURCE
/* TPC-C's five transactions on SQLite, the same work tpcc.aup does
 * (benchmarks/tpcc/run.py builds the database: TPC-C's tables with their
 * composite primary keys, WITHOUT ROWID, and TPC-C's initial rows):
 *
 *     sqlite-tpcc DATABASE WAREHOUSES THREADS SECONDS full|normal|off [SEED [TRANSACTIONS]]
 *
 * THREADS terminals, each with its own connection and a home warehouse,
 * run the mix of clause 5.2.3 with the inputs of clause 2 for SECONDS, each
 * transaction BEGIN IMMEDIATE ... COMMIT (WAL, synchronous as given).
 * Departures as in tpcc.aup: customers by number, no texts carried along,
 * a customer's last order kept in c_last_order, Delivery a district a
 * transaction. With TRANSACTIONS each terminal stops after that many; the
 * inputs are drawn in tpcc-load's order. Prints one JSON line as tpcc-load
 * does. */
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { BUCKETS = 64 * 16, KINDS = 5 };
enum { NEW_ORDER, PAYMENT, ORDER_STATUS, DELIVERY, STOCK_LEVEL };
static const char *const names[KINDS] = {"new_order", "payment", "order_status", "delivery", "stock_level"};

static const char *database, *synchronous;
static int warehouses;
static double seconds;
static uint64_t seed, each;
static atomic_uint_fast64_t histogram[KINDS][BUCKETS], done[KINDS], failed[KINDS], undone;

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}
static unsigned bucket(uint64_t us) {
    if (us < 16) return (unsigned)us;
    unsigned power = 63 - (unsigned)__builtin_clzll(us);
    unsigned index = (power - 3) * 16 + (unsigned)((us >> (power - 4)) & 15);
    return index < BUCKETS ? index : BUCKETS - 1;
}
static uint64_t bucket_value(unsigned index) {
    if (index < 16) return index;
    unsigned power = index / 16 + 3;
    return ((uint64_t)16 + index % 16) << (power - 4);
}
static uint64_t percentile(atomic_uint_fast64_t *counts, uint64_t total, double p) {
    uint64_t want = (uint64_t)((double)total * p), seen = 0;
    for (unsigned i = 0; i < BUCKETS; ++i) {
        seen += counts[i];
        if (seen > want) return bucket_value(i);
    }
    return 0;
}

static uint64_t next_random(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *state = x;
}
static int uniform(uint64_t *state, int low, int high) { return low + (int)(next_random(state) % (uint64_t)(high - low + 1)); }
static int nurand(uint64_t *state, int a, int low, int high, int c) {
    return ((uniform(state, 0, a) | uniform(state, low, high)) + c) % (high - low + 1) + low;
}

static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *statement;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", sql, sqlite3_errmsg(db));
        exit(1);
    }
    return statement;
}

/* A statement stepped without error: run to its end and reset (so it can
 * be bound again), or, given row, stepped once for its first row, which the
 * caller reads before resetting it. The first few errors are shown. */
static atomic_uint shown;
static bool step(sqlite3_stmt *statement, bool *row) {
    int result = sqlite3_step(statement);
    if (row) *row = result == SQLITE_ROW;
    while (result == SQLITE_ROW && !row) result = sqlite3_step(statement);
    bool ok = result == SQLITE_DONE || result == SQLITE_ROW;
    if (!ok && shown++ < 5) fprintf(stderr, "%s: %s\n", sqlite3_sql(statement), sqlite3_errmsg(sqlite3_db_handle(statement)));
    if (!row) sqlite3_reset(statement);
    return ok;
}

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *begin, *begin_read, *commit, *rollback;
    sqlite3_stmt *district_next, *district_advance, *customer_of, *item_price, *stock_of, *stock_take, *line_add, *order_add,
                 *new_order_add, *customer_last;
    sqlite3_stmt *warehouse_pay, *district_pay, *customer_pay, *history_add;
    sqlite3_stmt *customer_status, *lines_of;
    sqlite3_stmt *district_delivery, *delivery_advance, *new_order_remove, *order_carrier, *order_customer, *lines_deliver,
                 *lines_total, *customer_deliver;
    sqlite3_stmt *district_order, *stock_level;
} Terminal;

static void open_terminal(Terminal *t) {
    char pragmas[128];
    if (sqlite3_open(database, &t->db) != SQLITE_OK) { fprintf(stderr, "%s\n", sqlite3_errmsg(t->db)); exit(1); }
    sqlite3_busy_timeout(t->db, 60000);
    snprintf(pragmas, sizeof(pragmas), "PRAGMA journal_mode=WAL; PRAGMA synchronous=%s;", synchronous);
    if (sqlite3_exec(t->db, pragmas, NULL, NULL, NULL) != SQLITE_OK) { fprintf(stderr, "%s\n", sqlite3_errmsg(t->db)); exit(1); }
    sqlite3 *db = t->db;
    t->begin = prepare(db, "BEGIN IMMEDIATE");
    t->begin_read = prepare(db, "BEGIN");
    t->commit = prepare(db, "COMMIT");
    t->rollback = prepare(db, "ROLLBACK");
    t->district_next = prepare(db, "SELECT d_next_o_id FROM district WHERE d_w_id = ?1 AND d_id = ?2");
    t->district_advance = prepare(db, "UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_w_id = ?1 AND d_id = ?2");
    t->customer_of = prepare(db, "SELECT c_id FROM customer WHERE c_w_id = ?1 AND c_d_id = ?2 AND c_id = ?3");
    t->item_price = prepare(db, "SELECT i_price FROM item WHERE i_id = ?1");
    t->stock_of = prepare(db, "SELECT s_quantity FROM stock WHERE s_w_id = ?1 AND s_i_id = ?2");
    t->stock_take = prepare(db, "UPDATE stock SET s_quantity = ?3, s_ytd = s_ytd + ?4, s_order_cnt = s_order_cnt + 1, "
                                "s_remote_cnt = s_remote_cnt + ?5 WHERE s_w_id = ?1 AND s_i_id = ?2");
    t->line_add = prepare(db, "INSERT INTO order_line (ol_w_id, ol_d_id, ol_o_id, ol_number, ol_i_id, ol_supply_w_id, "
                              "ol_delivery_d, ol_quantity, ol_amount) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 0, ?7, ?8)");
    t->order_add = prepare(db, "INSERT INTO orders (o_w_id, o_d_id, o_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local) "
                               "VALUES (?1, ?2, ?3, ?4, ?5, 0, ?6, ?7)");
    t->new_order_add = prepare(db, "INSERT INTO new_order (no_w_id, no_d_id, no_o_id) VALUES (?1, ?2, ?3)");
    t->customer_last = prepare(db, "UPDATE customer SET c_last_order = ?4 WHERE c_w_id = ?1 AND c_d_id = ?2 AND c_id = ?3");
    t->warehouse_pay = prepare(db, "UPDATE warehouse SET w_ytd = w_ytd + ?2 WHERE w_id = ?1");
    t->district_pay = prepare(db, "UPDATE district SET d_ytd = d_ytd + ?3 WHERE d_w_id = ?1 AND d_id = ?2");
    t->customer_pay = prepare(db, "UPDATE customer SET c_balance = c_balance - ?4, c_ytd_payment = c_ytd_payment + ?4, "
                                  "c_payment_cnt = c_payment_cnt + 1 WHERE c_w_id = ?1 AND c_d_id = ?2 AND c_id = ?3");
    t->history_add = prepare(db, "INSERT INTO history (h_c_w_id, h_c_d_id, h_c_id, h_w_id, h_d_id, h_date, h_amount) "
                                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    t->customer_status = prepare(db, "SELECT c_balance, c_last_order FROM customer WHERE c_w_id = ?1 AND c_d_id = ?2 AND c_id = ?3");
    t->lines_of = prepare(db, "SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line "
                              "WHERE ol_w_id = ?1 AND ol_d_id = ?2 AND ol_o_id = ?3");
    t->district_delivery = prepare(db, "SELECT d_next_delivery, d_next_o_id FROM district WHERE d_w_id = ?1 AND d_id = ?2");
    t->delivery_advance = prepare(db, "UPDATE district SET d_next_delivery = d_next_delivery + 1 WHERE d_w_id = ?1 AND d_id = ?2");
    t->new_order_remove = prepare(db, "DELETE FROM new_order WHERE no_w_id = ?1 AND no_d_id = ?2 AND no_o_id = ?3");
    t->order_carrier = prepare(db, "UPDATE orders SET o_carrier_id = ?4 WHERE o_w_id = ?1 AND o_d_id = ?2 AND o_id = ?3");
    t->order_customer = prepare(db, "SELECT o_c_id FROM orders WHERE o_w_id = ?1 AND o_d_id = ?2 AND o_id = ?3");
    t->lines_deliver = prepare(db, "UPDATE order_line SET ol_delivery_d = ?4 WHERE ol_w_id = ?1 AND ol_d_id = ?2 AND ol_o_id = ?3");
    t->lines_total = prepare(db, "SELECT sum(ol_amount) FROM order_line WHERE ol_w_id = ?1 AND ol_d_id = ?2 AND ol_o_id = ?3");
    t->customer_deliver = prepare(db, "UPDATE customer SET c_balance = c_balance + ?4, c_delivery_cnt = c_delivery_cnt + 1 "
                                      "WHERE c_w_id = ?1 AND c_d_id = ?2 AND c_id = ?3");
    t->district_order = prepare(db, "SELECT d_next_o_id FROM district WHERE d_w_id = ?1 AND d_id = ?2");
    t->stock_level = prepare(db, "SELECT count(DISTINCT s_i_id) FROM order_line, stock WHERE ol_w_id = ?1 AND ol_d_id = ?2 "
                                 "AND ol_o_id < ?3 AND ol_o_id >= ?3 - 20 AND s_w_id = ?1 AND s_i_id = ol_i_id AND s_quantity < ?4");
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec * 1e3 + (double)(t.tv_nsec / 1000000);
}

#define RUN(s) step(s, NULL)

/* A read-only transaction: Order-Status or Stock-Level, each on one snapshot. */
static bool read_only(Terminal *t, int kind, int w, int d, uint64_t *r) {
    bool row;
    if (kind == ORDER_STATUS) {
        sqlite3_stmt *s = t->customer_status;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int(s, 3, nurand(r, 1023, 1, 3000, 259));
        if (!step(s, &row) || !row) return false;
        sqlite3_int64 order = sqlite3_column_int64(s, 1);
        (void)sqlite3_column_double(s, 0);
        sqlite3_reset(s);
        s = t->lines_of;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int64(s, 3, order);
        int result;
        while ((result = sqlite3_step(s)) == SQLITE_ROW) (void)sqlite3_column_double(s, 3);
        sqlite3_reset(s);
        return result == SQLITE_DONE;
    }
    sqlite3_stmt *s = t->district_order;
    sqlite3_reset(s);
    sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d);
    if (!step(s, &row) || !row) return false;
    sqlite3_int64 next = sqlite3_column_int64(s, 0);
    sqlite3_reset(s);
    s = t->stock_level;
    sqlite3_reset(s);
    sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int64(s, 3, next), sqlite3_bind_int(s, 4, uniform(r, 10, 20));
    bool ok = step(s, &row) && row;
    if (ok) (void)sqlite3_column_int64(s, 0);
    sqlite3_reset(s);
    return ok;
}

/* One transaction of kind; false when it failed (an unused item's New-Order is undone and counts in undone). */
static bool transact(Terminal *t, int kind, int w, int d, uint64_t *r) {
    bool row;
    if (kind == ORDER_STATUS || kind == STOCK_LEVEL) {
        if (!RUN(t->begin_read)) return false;
        bool ok = read_only(t, kind, w, d, r);
        return RUN(t->commit) && ok;
    }
    if (!RUN(t->begin)) return false;
    bool ok = true;
    if (kind == NEW_ORDER) {
        int c = nurand(r, 1023, 1, 3000, 259), count = uniform(r, 5, 15), local = 1;
        bool unused = uniform(r, 1, 100) == 1;
        sqlite3_stmt *s = t->district_next;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d);
        ok = step(s, &row) && row;
        sqlite3_int64 order = ok ? sqlite3_column_int64(s, 0) : 0;
        sqlite3_reset(s);
        sqlite3_bind_int(t->district_advance, 1, w), sqlite3_bind_int(t->district_advance, 2, d);
        ok = ok && RUN(t->district_advance);
        s = t->customer_of;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int(s, 3, c);
        ok = ok && step(s, &row) && row;
        sqlite3_reset(s);
        for (int n = 1; ok && n <= count; ++n) {
            int item = unused && n == count ? 100001 : nurand(r, 8191, 1, 100000, 7911);
            int supply = warehouses > 1 && uniform(r, 1, 100) == 1 ? uniform(r, 1, warehouses) : w, quantity = uniform(r, 1, 10);
            s = t->item_price;
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, item);
            if (!step(s, &row) || !row) { /* an unused item: the order is undone */
                sqlite3_reset(s);
                RUN(t->rollback);
                if (unused) ++undone;
                return unused;
            }
            double price = sqlite3_column_double(s, 0);
            sqlite3_reset(s);
            s = t->stock_of;
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, supply), sqlite3_bind_int(s, 2, item);
            ok = step(s, &row) && row;
            int stocked = ok ? sqlite3_column_int(s, 0) : 0;
            sqlite3_reset(s);
            int remote = supply != w;
            if (remote) local = 0;
            s = t->stock_take;
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, supply), sqlite3_bind_int(s, 2, item);
            sqlite3_bind_int(s, 3, stocked >= quantity + 10 ? stocked - quantity : stocked - quantity + 91);
            sqlite3_bind_int(s, 4, quantity), sqlite3_bind_int(s, 5, remote);
            ok = ok && RUN(s);
            s = t->line_add;
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int64(s, 3, order), sqlite3_bind_int(s, 4, n);
            sqlite3_bind_int(s, 5, item), sqlite3_bind_int(s, 6, supply), sqlite3_bind_int(s, 7, quantity);
            sqlite3_bind_double(s, 8, quantity * price);
            ok = ok && RUN(s);
        }
        s = t->order_add;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int64(s, 3, order), sqlite3_bind_int(s, 4, c);
        sqlite3_bind_double(s, 5, now_ms()), sqlite3_bind_int(s, 6, count), sqlite3_bind_int(s, 7, local);
        ok = ok && RUN(s);
        s = t->new_order_add;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int64(s, 3, order);
        ok = ok && RUN(s);
        s = t->customer_last;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int(s, 3, c), sqlite3_bind_int64(s, 4, order);
        ok = ok && RUN(s);
    } else if (kind == PAYMENT) {
        int cw = w, cd = d, c = nurand(r, 1023, 1, 3000, 259);
        if (warehouses > 1 && uniform(r, 1, 100) > 85) {
            do cw = uniform(r, 1, warehouses); while (cw == w);
            cd = uniform(r, 1, 10);
        }
        double amount = uniform(r, 100, 500000) / 100.0;
        sqlite3_stmt *s = t->warehouse_pay;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, w), sqlite3_bind_double(s, 2, amount);
        ok = RUN(s);
        s = t->district_pay;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_double(s, 3, amount);
        ok = ok && RUN(s);
        s = t->customer_pay;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, cw), sqlite3_bind_int(s, 2, cd), sqlite3_bind_int(s, 3, c), sqlite3_bind_double(s, 4, amount);
        ok = ok && RUN(s) && sqlite3_changes(t->db) == 1;
        s = t->history_add;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, cw), sqlite3_bind_int(s, 2, cd), sqlite3_bind_int(s, 3, c), sqlite3_bind_int(s, 4, w);
        sqlite3_bind_int(s, 5, d), sqlite3_bind_double(s, 6, now_ms()), sqlite3_bind_double(s, 7, amount);
        ok = ok && RUN(s);
    } else { /* DELIVERY, one district */
        int carrier = uniform(r, 1, 10);
        sqlite3_stmt *s = t->district_delivery;
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d);
        ok = step(s, &row) && row;
        sqlite3_int64 order = ok ? sqlite3_column_int64(s, 0) : 0, next = ok ? sqlite3_column_int64(s, 1) : 0;
        sqlite3_reset(s);
        if (ok && order < next) {
            sqlite3_bind_int(t->delivery_advance, 1, w), sqlite3_bind_int(t->delivery_advance, 2, d);
            ok = RUN(t->delivery_advance);
            s = t->new_order_remove;
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int64(s, 3, order);
            ok = ok && RUN(s) && sqlite3_changes(t->db) == 1;
            s = t->order_carrier;
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int64(s, 3, order), sqlite3_bind_int(s, 4, carrier);
            ok = ok && RUN(s);
            s = t->order_customer;
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int64(s, 3, order);
            ok = ok && step(s, &row) && row;
            int c = ok ? sqlite3_column_int(s, 0) : 0;
            sqlite3_reset(s);
            s = t->lines_deliver;
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int64(s, 3, order), sqlite3_bind_double(s, 4, now_ms());
            ok = ok && RUN(s);
            s = t->lines_total;
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int64(s, 3, order);
            ok = ok && step(s, &row) && row;
            double total = ok ? sqlite3_column_double(s, 0) : 0;
            sqlite3_reset(s);
            s = t->customer_deliver;
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, w), sqlite3_bind_int(s, 2, d), sqlite3_bind_int(s, 3, c), sqlite3_bind_double(s, 4, total);
            ok = ok && RUN(s);
        }
    }
    if (ok && RUN(t->commit)) return true;
    RUN(t->rollback);
    return false;
}

static void *terminal(void *argument) {
    int index = (int)(uintptr_t)argument;
    uint64_t random = 0x9e3779b97f4a7c15u * (uint64_t)(index + 1) ^ seed * 0xbf58476d1ce4e5b9u;
    Terminal t;
    open_terminal(&t);
    int home = index % warehouses + 1;
    uint64_t stop = now_us() + (uint64_t)(seconds * 1e6);
    for (uint64_t left = each; left && now_us() < stop; --left) {
        int d = uniform(&random, 1, 10), roll = uniform(&random, 1, 100);
        int kind = roll <= 45 ? NEW_ORDER : roll <= 88 ? PAYMENT : roll <= 92 ? ORDER_STATUS : roll <= 96 ? DELIVERY : STOCK_LEVEL;
        uint64_t started = now_us();
        bool ok = transact(&t, kind, home, d, &random);
        ++done[kind];
        if (!ok) ++failed[kind];
        ++histogram[kind][bucket(now_us() - started)];
    }
    for (sqlite3_stmt *s; (s = sqlite3_next_stmt(t.db, NULL));) sqlite3_finalize(s);
    sqlite3_close(t.db);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fputs("sqlite-tpcc DATABASE WAREHOUSES THREADS SECONDS full|normal|off [SEED [TRANSACTIONS]]\n", stderr);
        return 2;
    }
    database = argv[1];
    warehouses = atoi(argv[2]);
    int threads = atoi(argv[3]);
    seconds = atof(argv[4]);
    synchronous = argv[5];
    seed = argc > 6 ? strtoull(argv[6], NULL, 10) : 1;
    each = argc > 7 ? strtoull(argv[7], NULL, 10) : UINT64_MAX;
    pthread_t *thread = calloc((size_t)threads, sizeof(*thread));
    uint64_t start = now_us();
    for (int i = 0; i < threads; ++i) pthread_create(&thread[i], NULL, terminal, (void *)(uintptr_t)i);
    for (int i = 0; i < threads; ++i) pthread_join(thread[i], NULL);
    double elapsed = (double)(now_us() - start) / 1e6;
    uint64_t total = 0;
    for (int k = 0; k < KINDS; ++k) total += done[k];
    printf("{\"system\":\"sqlite %s\",\"synchronous\":\"%s\",\"warehouses\":%d,\"terminals\":%d,\"transactions_per_second\":%.0f,"
           "\"new_orders_per_minute\":%.0f,\"undone_new_orders\":%llu",
           sqlite3_libversion(), synchronous, warehouses, threads, (double)total / elapsed,
           (double)(done[NEW_ORDER] - undone) * 60 / elapsed, (unsigned long long)undone);
    for (int k = 0; k < KINDS; ++k)
        printf(",\"%s\":{\"done\":%llu,\"failed\":%llu,\"p50_us\":%llu,\"p99_us\":%llu}", names[k], (unsigned long long)done[k],
               (unsigned long long)failed[k], (unsigned long long)percentile(histogram[k], done[k], 0.5),
               (unsigned long long)percentile(histogram[k], done[k], 0.99));
    puts("}");
    return 0;
}
