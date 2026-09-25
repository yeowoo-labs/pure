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

PureData pure_tree_data(const char *path) {
    return (PureData){.context = (void *)path, .open = open_document, .version = version,
                      .read = read, .splice = splice, .commit = commit, .close = close_document};
}
