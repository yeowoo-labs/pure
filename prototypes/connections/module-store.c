#include "module-store.h"

static PureResult open_document(void *context, uint32_t collection, const char *key,
                                void **document, int64_t *revision) {
    (void)collection; /* v2 modules have one collection: the store's key space */
    PureDraft *draft;
    PureResult result = pure_draft_open(context, key, &draft);
    if (result != PURE_OK) return result;
    pure_draft_hold(draft, true); /* values read in the event stay readable */
    *document = draft;
    *revision = pure_draft_revision(draft);
    return PURE_OK;
}

static PureVersion version(void *context, void *document) {
    (void)context;
    return pure_draft_version(document);
}

static PureResult read(void *context, void *document, PureVersion at, size_t offset, size_t length,
                       bool (*visit)(void *, PureSpan), void *visit_context) {
    (void)context;
    return pure_draft_read_version(document, at, offset, length, visit, visit_context);
}

static PureResult splice(void *context, void *document, size_t offset, size_t remove, PureSpan insert) {
    (void)context;
    return pure_draft_splice(document, offset, remove, insert);
}

static PureResult commit(void *context, void *document, int64_t *revision) {
    (void)context;
    return pure_draft_commit(document, revision);
}

static void close_document(void *context, void *document) {
    (void)context;
    pure_draft_close(document);
}

static PureResult state(void *context, const char *name, PureDraft **out) {
    return pure_state_open(context, name, out);
}

enum { SAVED = PURE_APP_PENDING + 2 * PURE_MODULE_STATES + 1 };

/* The documents and the app's changed states in one tree commit. */
static PureResult save(void *context, void *const *documents, size_t document_count, PureDraft *const *states,
                       size_t count, int64_t *revisions) {
    (void)context;
    PureDraft *drafts[SAVED] = {0};
    int64_t all[SAVED] = {0};
    if (document_count > PURE_APP_PENDING || count > SAVED - document_count) return PURE_LIMIT;
    for (size_t i = 0; i < document_count; ++i) drafts[i] = documents[i];
    for (size_t i = 0; i < count; ++i) drafts[document_count + i] = states[i];
    PureResult result = pure_draft_commit_all(drafts, document_count + count, all);
    for (size_t i = 0; i < document_count; ++i) revisions[i] = all[i];
    return result;
}

PureData pure_tree_data(const char *path) {
    return (PureData){.context = (void *)path, .open = open_document, .version = version,
                      .read = read, .splice = splice, .commit = commit, .close = close_document,
                      .state = state, .save = save};
}
