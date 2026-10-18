#include "module-store.h"

static PureResult state(void *context, const char *name, PureDraft **out) {
    return pure_state_open(context, name, out);
}

enum { SAVED = 2 * PURE_MODULE_STATES + 3 };

/* The app's changed states in one tree commit. */
static PureResult save(void *context, PureDraft *const *states, size_t count) {
    (void)context;
    int64_t revisions[SAVED];
    if (count > SAVED) return PURE_LIMIT;
    return pure_draft_commit_all(states, count, revisions);
}

static const char *path_directory(void *context) { return context; }

static const char *handle_directory(void *context) { return pure_store_path(context); }

static PureResult handle_state(void *context, const char *name, PureDraft **out) {
    return pure_store_state(context, name, out);
}

PureData pure_store_data(PureStore *store) {
    return (PureData){.context = store, .state = handle_state, .save = save, .directory = handle_directory};
}

PureData pure_tree_data(const char *path) {
    return (PureData){.context = (void *)path, .state = state, .save = save, .directory = path_directory};
}
