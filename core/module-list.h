#ifndef PURE_MODULE_LIST_H
#define PURE_MODULE_LIST_H
#include "result.h"
#include "state.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Executor paths that run once per module or app (loading, verifying,
 * starting, replacing, reading stored states) are built for size; events
 * stay built for speed. */
#if defined(__GNUC__) || defined(__clang__)
#define PURE_COLD __attribute__((cold))
#else
#define PURE_COLD
#endif

/* Runtime-owned lists of records for the module executor. An item is one
 * cell per field: a number, or text bytes the list owns. Every change made
 * during an event is logged; a failed event undoes the log in reverse and a
 * successful one frees what it replaced. */
typedef struct { double num; unsigned char *text; size_t size; } PureCell;
typedef struct PureList PureList;
/* An event's list changes; a removed item's cells wait in cells. */
typedef struct {
    void *entries;
    size_t count, capacity;
    PureCell *cells;
    size_t cell_count, cell_capacity;
} PureUndo;
/* Nodes and texts a list let go while a shared copy may still read them. */
typedef struct { void *entries; size_t count, capacity; } PureRetired;
/* Items one after another, as a leaf of the list keeps them: positions start
 * to start + count - 1, each field's cells side by side (a number field's as
 * numbers, a text field's as spans). */
typedef struct {
    size_t start;
    unsigned count;
    union { const double *numbers; const PureSpan *texts; } column[16];
    /* Where the run is in the list's tree, for the next one. */
    const void *path[16];
    uint8_t taken[16];
    unsigned depth;
} PureRun;

/* A list holds up to PURE_LIST_ITEMS items, a run up to PURE_LIST_RUN. */
enum { PURE_LIST_ITEMS = 1 << 24, PURE_LIST_RUN = 32 };

/* text_fields: a field's kind, 0 a number, 1 a text (UTF-8, checked when
 * read), 2 bytes (any: a runtime list's). */
PureList *pure_list_new(unsigned fields, const uint8_t *text_fields);
void pure_list_free(PureList *);
/* Let every item go (retired where a shared copy may read it). */
void pure_list_clear(PureList *);
size_t pure_list_count(const PureList *);
/* Share the items as they are with a read-only copy (for readers on other
 * threads), without copying them: from then on the list copies the nodes it
 * changes, and nodes and texts it lets go go to retired instead of being
 * freed, since the copy may still read them. The copy is freed with
 * pure_list_free, on any thread, when its last reference goes
 * (pure_list_hold takes one more). Only between events (no change waits to
 * be committed or undone). epoch numbers the copy (as a snapshot of an app
 * numbers its lists' copies, one number for all of them): a node or text
 * the list makes after it has the epoch, and a copy of epoch e reads only
 * what was made before e. */
PureList *pure_list_share(PureList *, PureRetired *retired, uint64_t epoch);
/* A copy made before and still current (the list unchanged since) is read
 * as of epoch too: what the list lets go from now on is retired. */
void pure_list_mark(PureList *, uint64_t epoch);
/* Of what was retired, what a copy of epoch may still read (made before
 * it) goes to kept (none when kept is NULL), and the rest to gone, for
 * pure_retired_free (later, and where it pleases): from is left empty. */
void pure_retired_pass(PureRetired *from, uint64_t epoch, PureRetired *kept, PureRetired *gone);
void pure_list_hold(PureList *copy);
/* Whether copy shares list's items as they are now. */
bool pure_list_current(const PureList *copy, const PureList *list);
/* Which list, and its version: the same pair means the same items. */
void pure_list_stamp(const PureList *, uint64_t *id, uint64_t *version);
/* Free what was retired while no copy that shared it remains. */
void pure_retired_free(PureRetired *);
/* Insert a copy of cells (text bytes are copied) before position at. A NULL
 * log means the change is not undone. */
PureResult pure_list_insert(PureList *, size_t at, const PureCell *cells, PureUndo *);
PureResult pure_list_remove(PureList *, size_t at, PureUndo *);
PureResult pure_list_set(PureList *, size_t at, unsigned field, PureCell value, PureUndo *);
PureCell pure_list_get(const PureList *, size_t at, unsigned field);
/* The run holding position at (false past the end), and the run after
 * run (false at the end): for reading item after item without finding each.
 * Valid until the list changes. */
bool pure_list_run(const PureList *, size_t at, PureRun *);
bool pure_list_next(const PureList *, PureRun *);
/* In a list ordered by field (a keyed list): the first position whose item's
 * field does not order below key (numbers by value, texts by bytes), and
 * whether that item's field is key. */
size_t pure_list_seek(const PureList *, unsigned field, PureCell key, bool *found);
/* The field a keyed list keeps its items in order of: when it is a number
 * field, seek by it reads each level's keys from one node (field -1, or a
 * text field: none). ALLOCATION leaves the list searched as if it had none. */
PureResult pure_list_keyed(PureList *, int field);
/* The list's items ordered by field, as a keyed list keeps them: *out is NULL
 * when they already are, or a new list in that order with moved[position
 * before] = position after for every item. CONFLICT when two items have the
 * same key (nothing is made). */
PureResult pure_list_sorted(const PureList *, unsigned field, PureList **out, size_t *moved);
/* The stored form of items: each item's fields in order, a number as its
 * binary64 bits (little-endian), a text as a u32le length and its bytes.
 * pure_list_size is the bytes of all the items so stored, and
 * pure_list_encode writes them to out. */
size_t pure_list_size(const PureList *);
void pure_list_encode(const PureList *, unsigned char *out);
/* The items from position *at on, as many whole ones as capacity holds,
 * into out: their bytes (0 at the end, or with *needed the bytes of the one
 * item that does not fit), *at moved past them. */
size_t pure_list_encode_part(const PureList *, size_t *at, unsigned char *out, size_t capacity, size_t *needed);
/* Append the items stored in bytes; each field is checked. */
PureResult pure_list_read(PureList *, const unsigned char *bytes, size_t size);
/* A stored list journals its changes (from pure_list_journal(list, true)
 * on): every insert, remove and set adds a record, a u8 kind (1 insert, 2
 * remove, 3 set) and the u32le position, then an insert's item or a set's
 * u8 field and cell in stored form. A change there is no room to record does
 * not happen. pure_list_changes are the records since pure_list_forget last
 * kept none; forgetting down to a length taken earlier drops the records of
 * changes undone since (an undo does not journal). pure_list_replay makes
 * the changes of records again, in order: a list read from its stored items
 * and given its journal is the list that wrote them. */
void pure_list_journal(PureList *, bool journaled);
PureSpan pure_list_changes(const PureList *);
void pure_list_forget(PureList *, size_t size);
PureResult pure_list_replay(PureList *, const unsigned char *bytes, size_t size);
/* Undo the changes logged after mark (a count taken earlier), newest first. */
void pure_undo_rollback(PureUndo *, size_t mark);
void pure_undo_commit(PureUndo *);
void pure_undo_free(PureUndo *);
#endif
