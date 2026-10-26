#include "module-store.h"
#include <stdlib.h>
#include <string.h>

/* The app's changed states in one tree commit (their revisions on the stack for a few). */
static PureResult save(void *context, PureDraft *const *states, size_t count) {
    (void)context;
    int64_t few[67], *revisions = count <= 67 ? few : malloc(count * sizeof(*revisions));
    if (!revisions) return PURE_ALLOCATION;
    PureResult result = pure_draft_commit_all(states, count, revisions);
    if (revisions != few) free(revisions);
    return result;
}

static const char *handle_directory(void *context) { return pure_store_path(context); }

static PureResult handle_state(void *context, const char *name, PureDraft **out) {
    return pure_store_state(context, name, out);
}

PureData pure_store_data(PureStore *store) {
    return (PureData){.context = store, .state = handle_state, .save = save, .directory = handle_directory};
}

/* An app started by path holds the store as a handle while it runs: its
 * process is the store's writer, so another process's app on the store is
 * refused before it reads its log. */
typedef struct { char *path; PureStore *store; } Held;

static PureResult hold_store(void *context) {
    Held *held = context;
    return !held ? PURE_ALLOCATION : pure_store_open(held->path, &held->store);
}

static void release_store(void *context) {
    Held *held = context;
    if (!held) return;
    pure_store_close(held->store);
    free(held->path);
    free(held);
}

static PureResult held_state(void *context, const char *name, PureDraft **out) {
    return pure_store_state(((Held *)context)->store, name, out);
}

static const char *held_directory(void *context) { return ((Held *)context)->path; }

PureData pure_tree_data(const char *path) {
    Held *held = calloc(1, sizeof(*held));
    size_t length = strlen(path) + 1;
    if (held && (held->path = malloc(length))) memcpy(held->path, path, length);
    else free(held), held = NULL;
    return (PureData){.context = held, .state = held_state, .save = save, .directory = held_directory, .hold = hold_store,
                      .release = release_store};
}
