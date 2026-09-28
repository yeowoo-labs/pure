#ifndef PURE_LOG_H
#define PURE_LOG_H
#include "flow.h"
#include "result.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An app's log beside its store (store/store-log.c): an entry a save, its
 * number (one more than the entry before), whether the save also committed
 * the tree, and its payload. An entry counts once pure_log_append returns
 * PURE_OK: it is on the device. One thread uses a log. */
typedef struct PureLog PureLog;
/* The log in directory (made empty when it has none). The entries found are
 * checked (digests, numbers following on) up to the first that is not. */
PureResult pure_log_open(const char *directory, PureLog **out);
void pure_log_close(PureLog *);
/* The last entry's number (0: none). */
uint64_t pure_log_last(const PureLog *);
/* The entries from number from on (0: every entry kept), in order; payloads
 * stay valid until the next append. STORE_ERROR when an entry from `from` on
 * is missing; a visit that returns false stops. Only before appending. */
PureResult pure_log_read(PureLog *, uint64_t from,
                         bool (*visit)(void *, uint64_t seq, bool tree, PureSpan payload), void *context);
/* Entries after seq no longer count: the next append follows seq, over them. */
void pure_log_end(PureLog *, uint64_t seq);
/* Append the entry numbered seq (one more than the last) and make it durable:
 * UNCERTAIN when writing or syncing failed (it may or may not be there). */
PureResult pure_log_append(PureLog *, uint64_t seq, bool tree, PureSpan payload);
/* The last appended entry no longer counts (its save failed after it was
 * written): the next append takes its number and its place. */
void pure_log_retract(PureLog *);
/* Entries up to seq are no longer read: their segments may be used again. */
void pure_log_release(PureLog *, uint64_t seq);
/* Bytes of the segments that hold entries not released. */
uint64_t pure_log_bytes(const PureLog *);

/* A list's image: a file of directory named after the list, replaced whole
 * (written beside it, synced, renamed over it): pure_image_write writes the
 * chunks next gives, one after another, until an empty one (next false:
 * ALLOCATION, nothing replaced); pure_image_read gives them back (malloc'd;
 * NOT_FOUND when there is none, STORE_ERROR when damaged). The bytes of the
 * image are digested: a file cut short or changed is refused. */
PureResult pure_image_write(const char *directory, const char *name, bool (*next)(void *, PureSpan *chunk), void *context);
PureResult pure_image_read(const char *directory, const char *name, unsigned char **bytes, size_t *size);
PureResult pure_image_remove(const char *directory, const char *name);
#endif
