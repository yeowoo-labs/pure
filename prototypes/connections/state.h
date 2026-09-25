#ifndef PURE_STATE_H
#define PURE_STATE_H
#include "flow.h"
#include "result.h"
#include <stdint.h>

/* Runtime-owned working versions of stored values (selected tree store).
 * A draft starts at the committed version of a key and changes only in
 * memory: an edit copies the leaf and path it touches and shares every other
 * extent with the stored version. An app therefore keeps no copy of the value
 * and never compares a copy with storage to find what it changed. Commit
 * appends only the draft's new extents. One owner thread per draft. */
typedef struct PureDraft PureDraft;

/* A missing key opens as an empty draft at revision 0; commit creates it. */
PureResult pure_draft_open(const char *path, const char *key, PureDraft **out);
/* Memory only. Fails without change on a bad range or when memory runs out. */
PureResult pure_draft_splice(PureDraft *, size_t offset, size_t remove, PureSpan insert);
size_t pure_draft_size(const PureDraft *);
/* The committed revision this draft is based on; 0 if the key was absent. */
int64_t pure_draft_revision(const PureDraft *);
/* Visit [offset, offset + length) in order as borrowed extents, without
 * copying. Returning false stops early. Spans stay valid until the next
 * splice, commit or close of this draft. */
PureResult pure_draft_read(const PureDraft *, size_t offset, size_t length,
                           bool (*visit)(void *, PureSpan), void *context);
/* Publish if the key is still at the draft's base revision. On success the
 * draft continues from the new revision; on any failure it is unchanged. */
PureResult pure_draft_commit(PureDraft *, int64_t *revision);
void pure_draft_close(PureDraft *);
#endif
