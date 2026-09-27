#define _GNU_SOURCE
/* pgbench's TPC-B-like transaction on SQLite, the same bank as bank.aup for
 * comparing with Pure (benchmarks/bank/run.py):
 *
 *     sqlite-bank DATABASE ACCOUNTS THREADS SECONDS full|normal|off
 *
 * makes pgbench's tables (a branch, 10 tellers and 100,000 accounts a scale;
 * ACCOUNTS, at least one scale), then THREADS threads, each with its own
 * connection, run the transaction for SECONDS: BEGIN IMMEDIATE; UPDATE
 * accounts; SELECT the balance; UPDATE tellers; UPDATE branches; INSERT into
 * history; COMMIT, with random ids and delta as pgbench picks them. WAL, with
 * synchronous FULL (each commit synced), NORMAL or OFF. Prints one JSON line. */
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *database, *synchronous;
static unsigned accounts, scale;
static double seconds;
static atomic_uint_fast64_t done, failed;
enum { BUCKETS = 64 * 16 };
static atomic_uint_fast64_t histogram[BUCKETS];

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

/* Log-linear buckets: 16 a power of two. */
static unsigned bucket(uint64_t us) {
    if (us < 16) return (unsigned)us;
    unsigned power = 63 - (unsigned)__builtin_clzll(us);
    return (power - 3) * 16 + (unsigned)((us >> (power - 4)) & 15);
}
static uint64_t bucket_value(unsigned index) {
    if (index < 16) return index;
    unsigned power = index / 16 + 3;
    return ((uint64_t)16 + index % 16) << (power - 4);
}
static uint64_t percentile(uint64_t total, double p) {
    uint64_t want = (uint64_t)((double)total * p), seen = 0;
    for (unsigned i = 0; i < BUCKETS; ++i) {
        seen += histogram[i];
        if (seen > want) return bucket_value(i);
    }
    return 0;
}

static sqlite3 *open_database(void) {
    sqlite3 *db;
    char pragmas[160];
    if (sqlite3_open(database, &db) != SQLITE_OK) { fprintf(stderr, "%s\n", sqlite3_errmsg(db)); exit(1); }
    sqlite3_busy_timeout(db, 60000);
    snprintf(pragmas, sizeof(pragmas), "PRAGMA journal_mode=WAL; PRAGMA synchronous=%s;", synchronous);
    if (sqlite3_exec(db, pragmas, NULL, NULL, NULL) != SQLITE_OK) { fprintf(stderr, "%s\n", sqlite3_errmsg(db)); exit(1); }
    return db;
}

static void run(sqlite3 *db, const char *sql) {
    char *error = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) { fprintf(stderr, "%s: %s\n", sql, error); exit(1); }
}

static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *statement;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) { fprintf(stderr, "%s\n", sqlite3_errmsg(db)); exit(1); }
    return statement;
}

static uint64_t next_random(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *state = x;
}

static bool step_done(sqlite3_stmt *statement) {
    int result = sqlite3_step(statement);
    sqlite3_reset(statement);
    return result == SQLITE_DONE || result == SQLITE_ROW;
}

static void *client(void *argument) {
    uint64_t random = 0x9e3779b97f4a7c15u * (uintptr_t)argument;
    sqlite3 *db = open_database();
    sqlite3_stmt *begin = prepare(db, "BEGIN IMMEDIATE"), *commit = prepare(db, "COMMIT"),
                 *rollback = prepare(db, "ROLLBACK"),
                 *account = prepare(db, "UPDATE pgbench_accounts SET abalance = abalance + ?1 WHERE aid = ?2"),
                 *balance = prepare(db, "SELECT abalance FROM pgbench_accounts WHERE aid = ?1"),
                 *teller = prepare(db, "UPDATE pgbench_tellers SET tbalance = tbalance + ?1 WHERE tid = ?2"),
                 *branch = prepare(db, "UPDATE pgbench_branches SET bbalance = bbalance + ?1 WHERE bid = ?2"),
                 *history = prepare(db, "INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) "
                                        "VALUES (?1, ?2, ?3, ?4, CURRENT_TIMESTAMP)");
    uint64_t stop = now_us() + (uint64_t)(seconds * 1e6);
    while (now_us() < stop) {
        int aid = (int)(next_random(&random) % accounts) + 1, tid = (int)(next_random(&random) % (10 * scale)) + 1,
            bid = (int)(next_random(&random) % scale) + 1, delta = (int)(next_random(&random) % 10001) - 5000;
        uint64_t started = now_us();
        sqlite3_bind_int(account, 1, delta), sqlite3_bind_int(account, 2, aid);
        sqlite3_bind_int(balance, 1, aid);
        sqlite3_bind_int(teller, 1, delta), sqlite3_bind_int(teller, 2, tid);
        sqlite3_bind_int(branch, 1, delta), sqlite3_bind_int(branch, 2, bid);
        sqlite3_bind_int(history, 1, tid), sqlite3_bind_int(history, 2, bid), sqlite3_bind_int(history, 3, aid),
            sqlite3_bind_int(history, 4, delta);
        bool ok = step_done(begin) && step_done(account) && step_done(balance) && step_done(teller) &&
                  step_done(branch) && step_done(history) && step_done(commit);
        if (!ok) {
            step_done(rollback);
            ++failed;
            continue;
        }
        ++done;
        ++histogram[bucket(now_us() - started)];
    }
    sqlite3_stmt *statements[] = {begin, commit, rollback, account, balance, teller, branch, history};
    for (size_t i = 0; i < sizeof(statements) / sizeof(*statements); ++i) sqlite3_finalize(statements[i]);
    sqlite3_close(db);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fputs("sqlite-bank DATABASE ACCOUNTS THREADS SECONDS full|normal|off\n", stderr);
        return 2;
    }
    database = argv[1];
    accounts = (unsigned)strtoul(argv[2], NULL, 10);
    unsigned threads = (unsigned)strtoul(argv[3], NULL, 10);
    seconds = atof(argv[4]);
    synchronous = argv[5];
    scale = accounts / 100000 ? accounts / 100000 : 1;
    uint64_t began = now_us();
    sqlite3 *db = open_database();
    run(db, "DROP TABLE IF EXISTS pgbench_accounts; DROP TABLE IF EXISTS pgbench_tellers;"
            "DROP TABLE IF EXISTS pgbench_branches; DROP TABLE IF EXISTS pgbench_history;"
            "CREATE TABLE pgbench_accounts (aid INTEGER PRIMARY KEY, bid INTEGER, abalance INTEGER, filler CHAR(84));"
            "CREATE TABLE pgbench_tellers (tid INTEGER PRIMARY KEY, bid INTEGER, tbalance INTEGER, filler CHAR(84));"
            "CREATE TABLE pgbench_branches (bid INTEGER PRIMARY KEY, bbalance INTEGER, filler CHAR(88));"
            "CREATE TABLE pgbench_history (tid INTEGER, bid INTEGER, aid INTEGER, delta INTEGER, mtime TIMESTAMP,"
            " filler CHAR(22));");
    run(db, "BEGIN");
    sqlite3_stmt *insert = prepare(db, "INSERT INTO pgbench_branches (bid, bbalance, filler) VALUES (?1, 0, '')");
    for (unsigned b = 1; b <= scale; ++b) sqlite3_bind_int(insert, 1, (int)b), step_done(insert);
    sqlite3_finalize(insert);
    insert = prepare(db, "INSERT INTO pgbench_tellers (tid, bid, tbalance, filler) VALUES (?1, ?2, 0, '')");
    for (unsigned t = 1; t <= 10 * scale; ++t) sqlite3_bind_int(insert, 1, (int)t), sqlite3_bind_int(insert, 2, (int)((t - 1) / 10 + 1)), step_done(insert);
    sqlite3_finalize(insert);
    insert = prepare(db, "INSERT INTO pgbench_accounts (aid, bid, abalance, filler) VALUES (?1, ?2, 0, '')");
    for (unsigned a = 1; a <= accounts; ++a)
        sqlite3_bind_int(insert, 1, (int)a), sqlite3_bind_int(insert, 2, (int)((a - 1) / 100000 + 1)), step_done(insert);
    sqlite3_finalize(insert);
    run(db, "COMMIT; PRAGMA wal_checkpoint(TRUNCATE);");
    double setup = (double)(now_us() - began) / 1e6;
    pthread_t *thread = calloc(threads, sizeof(*thread));
    uint64_t start = now_us();
    for (unsigned i = 0; i < threads; ++i) pthread_create(&thread[i], NULL, client, (void *)(uintptr_t)(i + 1));
    for (unsigned i = 0; i < threads; ++i) pthread_join(thread[i], NULL);
    double elapsed = (double)(now_us() - start) / 1e6;
    sqlite3_stmt *count = prepare(db, "SELECT count(*) FROM pgbench_history");
    sqlite3_step(count);
    long long history = sqlite3_column_int64(count, 0);
    sqlite3_finalize(count);
    sqlite3_close(db);
    printf("{\"sqlite\":\"%s\",\"synchronous\":\"%s\",\"accounts\":%u,\"threads\":%u,\"setup_s\":%.2f,"
           "\"transfers_per_second\":%.0f,\"p50_us\":%llu,\"p99_us\":%llu,\"failed\":%llu,\"history\":%lld}\n",
           sqlite3_libversion(), synchronous, accounts, threads, setup, (double)done / elapsed,
           (unsigned long long)percentile(done, 0.5), (unsigned long long)percentile(done, 0.99),
           (unsigned long long)failed, history);
    return history == (long long)done ? 0 : 1;
}
