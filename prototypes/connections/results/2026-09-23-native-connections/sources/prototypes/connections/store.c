#include "store.h"
#include <limits.h>
#include <string.h>

static PureResult error_result(int rc) {
    int code = rc & 255;
    return code == SQLITE_BUSY || code == SQLITE_LOCKED ? PURE_BUSY : PURE_STORE_ERROR;
}

static int execute(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static int open_db(const char *path, sqlite3 **db, int flags) {
    int rc = sqlite3_open_v2(path, db, flags | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) return rc;
    sqlite3_busy_timeout(*db, 0); /* No hidden wait/retry on this owner thread. */
    sqlite3_limit(*db, SQLITE_LIMIT_LENGTH, (int)PURE_OBJECT_LIMIT + 4096);
    return execute(*db, "PRAGMA cache_size=-64; PRAGMA synchronous=FULL;");
}

PureResult pure_store_init(const char *path) {
    sqlite3 *db = NULL;
    int rc = open_db(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    sqlite3_stmt *mode = NULL;
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode=WAL", -1, &mode, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(mode);
        if (rc == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(mode, 0);
            rc = name && strcmp((const char *)name, "wal") == 0 ? SQLITE_OK : SQLITE_ERROR;
        }
    }
    sqlite3_finalize(mode);
    if (rc == SQLITE_OK) rc = execute(db,
        "CREATE TABLE IF NOT EXISTS pure_objects("
        "key TEXT PRIMARY KEY, revision INTEGER NOT NULL CHECK(revision>0),"
        "body BLOB NOT NULL CHECK(typeof(body)='blob'));");
    if (db) sqlite3_close(db);
    return rc == SQLITE_OK ? PURE_OK : error_result(rc);
}

void pure_store_release(PureStoredView *view) {
    sqlite3_finalize(view->statement);
    if (view->db) {
        execute(view->db, "ROLLBACK");
        sqlite3_close(view->db);
    }
    *view = (PureStoredView){0};
}

PureResult pure_store_get(const char *path, const char *key, size_t available,
                          PureStoredView *out) {
    *out = (PureStoredView){0};
    int rc = open_db(path, &out->db, SQLITE_OPEN_READONLY);
    if (rc == SQLITE_OK) rc = execute(out->db, "BEGIN");
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(out->db,
        "SELECT revision,length(body) FROM pure_objects WHERE key=?", -1, &out->statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(out->statement, 1, key, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(out->statement);
    PureResult result = PURE_STORE_ERROR;
    if (rc == SQLITE_DONE) { result = PURE_NOT_FOUND; goto fail; }
    if (rc != SQLITE_ROW) { result = error_result(rc); goto fail; }
    out->revision = sqlite3_column_int64(out->statement, 0);
    sqlite3_int64 size = sqlite3_column_int64(out->statement, 1);
    if (out->revision <= 0 || size < 0) goto fail;
    if (size > PURE_OBJECT_LIMIT) { result = PURE_LIMIT; goto fail; }
    if ((uint64_t)size > available) { result = PURE_BUDGET; goto fail; }
    sqlite3_finalize(out->statement);
    out->statement = NULL;
    /* Keep the read transaction so admission and body observe the same version.
     * The result remains borrowed until socket completion/abort finalizes it. */
    rc = sqlite3_prepare_v2(out->db, "SELECT body FROM pure_objects WHERE key=?", -1, &out->statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(out->statement, 1, key, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(out->statement);
    if (rc != SQLITE_ROW) { result = error_result(rc); goto fail; }
    if (sqlite3_column_type(out->statement, 0) != SQLITE_BLOB) goto fail;
    const unsigned char *data = sqlite3_column_blob(out->statement, 0);
    int bytes = sqlite3_column_bytes(out->statement, 0);
    if (bytes != size || (bytes && !data)) goto fail;
    out->body = (PureSpan){data, (size_t)bytes};
    return PURE_OK;
fail:
    pure_store_release(out);
    return result;
}

PureResult pure_store_put(const char *path, const char *key, int64_t expected,
                          PureSpan body, int64_t *revision) {
    *revision = 0;
    if (body.size > PURE_OBJECT_LIMIT || expected < 0) return PURE_LIMIT;
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    PureResult result = PURE_STORE_ERROR;
    int rc = open_db(path, &db, SQLITE_OPEN_READWRITE);
    if (rc == SQLITE_OK) rc = execute(db, "BEGIN IMMEDIATE");
    if (rc != SQLITE_OK) { result = error_result(rc); goto finish; }
    rc = sqlite3_prepare_v2(db, "SELECT revision FROM pure_objects WHERE key=?", -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, key, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) *revision = sqlite3_column_int64(statement, 0);
    else if (rc != SQLITE_DONE) { result = error_result(rc); goto finish; }
    sqlite3_finalize(statement);
    statement = NULL;
    if (*revision != expected) { result = PURE_CONFLICT; goto finish; }
    if (*revision == INT64_MAX) { result = PURE_LIMIT; goto finish; }
    const char *sql = *revision ?
        "UPDATE pure_objects SET body=?1,revision=?2 WHERE key=?3" :
        "INSERT INTO pure_objects(body,revision,key) VALUES(?1,?2,?3)";
    rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    static const unsigned char empty = 0;
    /* Do not copy at the binding boundary. Finalize before returning the input. */
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(statement, 1, body.size ? body.data : &empty,
                                              (int)body.size, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 2, *revision + 1);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 3, key, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    if (rc != SQLITE_DONE) { result = error_result(rc); goto finish; }
    rc = sqlite3_finalize(statement);
    statement = NULL;
    if (rc == SQLITE_OK) rc = execute(db, "COMMIT");
    if (rc != SQLITE_OK) { result = error_result(rc); goto finish; }
    ++*revision;
    result = PURE_OK; /* Acknowledgment is constructed only after COMMIT. */
finish:
    sqlite3_finalize(statement);
    if (db) {
        if (!sqlite3_get_autocommit(db)) execute(db, "ROLLBACK");
        sqlite3_close(db);
    }
    return result;
}
