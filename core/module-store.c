#include "module-store.h"
#include <stdlib.h>

static PureResult state(void *context, const char *name, PureDraft **out) {
    return pure_state_open(context, name, out);
}

/* The app's changed states in one tree commit (their revisions on the stack for a few). */
static PureResult save(void *context, PureDraft *const *states, size_t count) {
    (void)context;
    int64_t few[67], *revisions = count <= 67 ? few : malloc(count * sizeof(*revisions));
    if (!revisions) return PURE_ALLOCATION;
    PureResult result = pure_draft_commit_all(states, count, revisions);
    if (revisions != few) free(revisions);
    return result;
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
