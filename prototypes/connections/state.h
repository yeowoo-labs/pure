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

/* A working value with no stored base, for runtime-owned state such as an
 * app's text. It edits and reads like any draft but cannot be committed. */
PureDraft *pure_draft_new(void);
/* A missing key opens as an empty draft at revision 0; commit creates it. */
PureResult pure_draft_open(const char *path, const char *key, PureDraft **out);
/* A runtime-owned value of an app, such as a list or text state, stored
 * under a name that is never a document key (hosts listing documents do not
 * see it). name is a valid key of at most 63 bytes. */
PureResult pure_state_open(const char *path, const char *name, PureDraft **out);
/* A store kept open by one owner thread. Drafts opened through it share one
 * mapping of the newest version, and opening one checks only whether the file
 * changed; commits of its drafts map the new version once. It must outlive
 * its drafts. */
typedef struct PureStore PureStore;
PureResult pure_store_open(const char *path, PureStore **out);
void pure_store_close(PureStore *);
PureResult pure_store_draft(PureStore *, const char *key, PureDraft **out);
PureResult pure_store_state(PureStore *, const char *name, PureDraft **out);
/* Stored extents can outlive their draft: pure_draft_mapping takes a
 * reference to the mapping a draft opened through a handle reads its stored
 * nodes from (NULL for other drafts). Spans that mapping holds stay readable
 * until the reference is released, after the draft closes and after the
 * store moves on. Spans of a draft's unsaved edits are not in it. Use it on
 * the handle's owner thread. */
typedef struct PureMapping PureMapping;
PureMapping *pure_draft_mapping(const PureDraft *);
bool pure_mapping_holds(const PureMapping *, PureSpan);
void pure_mapping_release(PureMapping *);
/* Pinned, drafts open at the version found when pinning, without checking
 * the file each time (a host pins for one burst of requests); commits made
 * through the handle still move it on. */
PureResult pure_store_pin(PureStore *, bool pinned);
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
/* An immutable version of a draft's value. It stays readable while the draft
 * is held (reclamation is suspended) and until the next commit or close. */
typedef struct { uint64_t ref, bytes; } PureVersion;
PureVersion pure_draft_version(const PureDraft *);
PureResult pure_draft_read_version(const PureDraft *, PureVersion, size_t offset, size_t length,
                                   bool (*visit)(void *, PureSpan), void *context);
/* While held, edits keep every earlier version's nodes (one transaction). */
void pure_draft_hold(PureDraft *, bool held);
/* Abandon edits made since version was taken; the draft was held since then. */
void pure_draft_restore(PureDraft *, PureVersion);
/* Publish if the key is still at the draft's base revision. On success the
 * draft continues from the new revision; on any failure it is unchanged. */
PureResult pure_draft_commit(PureDraft *, int64_t *revision);
/* Publish drafts of one store in one commit: each key must still be at its
 * draft's base revision (a conflict names the stored revision in revisions),
 * and either every draft is published or none is. Keys must differ. */
PureResult pure_draft_commit_all(PureDraft *const *drafts, size_t count, int64_t *revisions);
/* Commits made so far in this process: groups formed, and requests in them. */
void pure_store_group_counts(unsigned long *groups, unsigned long *requests);
void pure_draft_close(PureDraft *);
#endif
