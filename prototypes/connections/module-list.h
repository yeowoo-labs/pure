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
typedef struct { void *entries; size_t count, capacity; } PureUndo;

enum { PURE_LIST_ITEMS = 65536 };

PureList *pure_list_new(unsigned fields, const uint8_t *text_fields);
void pure_list_free(PureList *);
size_t pure_list_count(const PureList *);
/* Insert a copy of cells (text bytes are copied) before position at. A NULL
 * log means the change is not undone. */
PureResult pure_list_insert(PureList *, size_t at, const PureCell *cells, PureUndo *);
PureResult pure_list_remove(PureList *, size_t at, PureUndo *);
PureResult pure_list_set(PureList *, size_t at, unsigned field, PureCell value, PureUndo *);
PureCell pure_list_get(const PureList *, size_t at, unsigned field);
/* In a list ordered by field (a keyed list): the first position whose item's
 * field does not order below key (numbers by value, texts by bytes), and
 * whether that item's field is key. */
size_t pure_list_seek(const PureList *, unsigned field, PureCell key, bool *found);
/* The list's items ordered by field, as a keyed list keeps them: *out is NULL
 * when they already are, or a new list in that order with moved[position
 * before] = position after for every item. CONFLICT when two items have the
 * same key (nothing is made). */
PureResult pure_list_sorted(const PureList *, unsigned field, PureList **out, size_t *moved);
/* A list may keep a stored copy in a draft: header bytes, then each item's
 * fields in order (a number as its binary64 bits, little-endian; a text as a
 * u32le length and its bytes). Every change splices the draft as well, and a
 * change the draft refuses (such as its size limit) does not happen. Undoing
 * an event changes only the list: the caller restores the draft's version. */
void pure_list_mirror(PureList *, PureDraft *, size_t header);
/* Store every item after the header (the draft holds just the header). */
PureResult pure_list_write(PureList *);
/* Append the items stored in bytes; each field is checked. Before mirroring. */
PureResult pure_list_read(PureList *, const unsigned char *bytes, size_t size);
/* Undo the changes logged after mark (a count taken earlier), newest first. */
void pure_undo_rollback(PureUndo *, size_t mark);
void pure_undo_commit(PureUndo *);
void pure_undo_free(PureUndo *);
#endif
