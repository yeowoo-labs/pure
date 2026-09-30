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
/* A snapshot's list (module.c): a state's, or a runtime list's (place
 * PURE_LIST_HEARD on) in a snapshot made with it (pure_save_snapshot;
 * UINT32_MAX: none). */
const PureList *pure_snapshot_list(const PureSnapshot *, uint32_t state);
PureSnapshot *pure_save_snapshot(PureApp *, uint32_t runtime);
/* A runtime list of a starting app (module.h, PureApp; place is
 * PURE_LIST_HEARD or PURE_LIST_OUTBOX): from its image with its logged
 * changes made again, or new (saved.rewrite: it needs an image, which
 * pure_save_runtime_image writes). */
PureResult pure_save_open_runtime(PureApp *, uint32_t place, PureList **out, PureSaved *saved);
/* A runtime list's image, holding the last save: list, or the app's when
 * NULL. The outbox's names the fields its sends were laid out with, m's
 * outputs' ("PLO1"): what they mean whatever the links hold. */
PureResult pure_save_runtime_image(PureApp *, const PureModule *m, uint32_t place, const PureList *list);
/* An empty runtime list. */
PureList *pure_runtime_list_new(uint32_t place);
/* Open the stored numbers, cursors and indices, and put each stored one of the
 * same name and kind that still fits into values, for the states of m that
 * kept marks as new (-1); texts and lists are m's. */
PureResult pure_save_open_values(PureApp *, const PureModule *m, PureValue *values, const int *kept,
                                 PureDraft *const *texts, PureList *const *lists);
/* The stored links of a starting app (module.h, PureApp), after its runtime
 * lists. The sends left in the outbox of an output whose fields changed are
 * recast to m's by name (pure_send_recast; sent_moved: the outbox needs an
 * image, as it does when m's outputs are not those its image names);
 * CONFLICT (app->why says) when one waits on an output m lacks, or on one
 * whose fields its store does not name (PLK3). Links stored before the
 * runtime lists kept the numbers heard and the sends: they go to them
 * (moved: they need images). */
PureResult pure_save_open_links(PureApp *, const PureModule *m, bool *heard_moved, bool *sent_moved);
/* Publish the documents and every state changed since the last save
 * (values are the states' numbers); revisions are the documents'. */
PureResult pure_save_commit(PureApp *, const PureValue *values, void *const *documents, size_t document_count,
                            int64_t *revisions);

/* A message's fields laid out as load lays them: in order, the fixed ones
 * and the variable ones' lengths in the prefix, but the last variable
 * field's, which takes what follows the others. The prefix's size. */
uint32_t pure_fields_lay_out(PureField *fields, uint32_t count);
/* A send's payload, laid out for from's fields, made anew for to's (both
 * laid out): each field takes the value of from's of the same name when
 * its type can hold it (u32, number and bool are numbers, bytes and text
 * are bytes; a number that is not a u32, one that is not 0 or 1 for a bool,
 * or bytes that are not UTF-8 for a text, cannot), else it is 0, false or
 * empty. The same rule moves a list's items (pure_list_migrate). A link
 * delivers only to a field of the same kind or a wider one
 * (pure_module_takes). STORE_ERROR when payload is not laid out by from. */
PureResult pure_send_recast(const PureField *from, uint32_t from_count, const PureField *to, uint32_t to_count,
                            PureSpan payload, unsigned char **out, size_t *size);

/* A record field's kind in a list, as a stored list names it too (PURE_CELL_...). */
uint8_t pure_record_kind(const PureModule *, uint32_t field);
/* A list with record's fields, taking from `from` (its fields' names and
 * kinds) each field of the same name and kind, or a number, a bool and an
 * i64 from each other when the value fits (pure_cell_as), and giving the
 * others their defaults (module.c). */
PureResult pure_list_migrate(PureList **out, const PureList *from, const char *const *names,
                             const uint8_t *kinds, unsigned count, const PureModule *m, uint32_t record);
#endif
