#ifndef PURE_MODULE_SAVE_H
#define PURE_MODULE_SAVE_H
#include "module.h"

/* App states a data host keeps (docs/design/008-module-format.md), internal
 * to the module executor. Each text state is a stored draft, each list state
 * an image and the changes logged since, and numbers, cursors and indices
 * are one stored value by name. A save publishes what changed, all or
 * nothing. */

bool pure_save_enabled(const PureApp *);
/* A text state of m: its stored value, or its initial constant when none. */
PureResult pure_save_open_text(PureApp *, const PureModule *m, uint32_t state, PureDraft **out, PureVersion *saved);
/* Stored lists (module-save.c): at a start, the log opened and read for the
 * lists (pure_save_open_log), each list opened from its image with its
 * logged changes made again (pure_save_open_list; saved says what its image
 * holds, or that it needs one: pure_save_image writes it, holding the last
 * save), then the start's reading let go (pure_save_close_start). */
typedef struct { uint64_t image, since; size_t logged; bool rewrite; } PureSaved;
PureResult pure_save_open_log(PureApp *);
PureResult pure_save_open_list(PureApp *, const PureModule *m, uint32_t state, PureList **out, PureDraft **legacy,
                               PureSaved *saved);
PureResult pure_save_image(PureApp *, const PureModule *m, uint32_t state, const PureList *list);
void pure_save_close_start(PureApp *);
/* The log keeps only the entries some list's image does not hold yet. */
void pure_save_release(PureApp *);
/* A snapshot's list (module.c): a state's, or the heard list's
 * (PURE_LIST_HEARD) in a snapshot made with it (pure_save_snapshot). */
const PureList *pure_snapshot_list(const PureSnapshot *, uint32_t state);
PureSnapshot *pure_save_snapshot(PureApp *, bool heard);
/* The heard list of a starting app (module.h, PureApp): from its image with
 * its logged changes made again, or new (saved.rewrite: it needs an image,
 * which pure_save_heard_image writes). */
PureResult pure_save_open_heard(PureApp *, PureList **out, PureSaved *saved);
PureResult pure_save_heard_image(PureApp *);
/* An empty heard list. */
PureList *pure_heard_new(void);
/* Open the stored numbers, cursors and indices, and put each stored one of the
 * same name and kind that still fits into values, for the states of m that
 * kept marks as new (-1); texts and lists are m's. */
PureResult pure_save_open_values(PureApp *, const PureModule *m, double *values, const int *kept,
                                 PureDraft *const *texts, PureList *const *lists);
/* The stored links of a starting app (module.h, PureApp); CONFLICT when an
 * output with sends left is missing or has other fields. Links stored before
 * the heard list kept the numbers heard: they go to it (moved: it needs an
 * image). */
PureResult pure_save_open_links(PureApp *, const PureModule *m, bool *moved);
/* Publish the documents and every state changed since the last save
 * (values are the states' numbers); revisions are the documents'. */
PureResult pure_save_commit(PureApp *, const double *values, void *const *documents, size_t document_count,
                            int64_t *revisions);

/* A list with record's fields, taking from `from` each field of the same name
 * and kind and giving the others their defaults (module.c). */
PureResult pure_list_migrate(PureList **out, const PureList *from, const char *const *names,
                             const uint8_t *text, unsigned count, const PureModule *m, uint32_t record);
#endif
