#ifndef PURE_MODULE_SAVE_H
#define PURE_MODULE_SAVE_H
#include "module.h"

/* App states a data host keeps (docs/design/008-module-format.md), internal
 * to the module executor. Each text state is a stored draft, each list state
 * a stored draft its list mirrors, and numbers, cursors and indices are one
 * stored value by name. A save publishes what changed, all or nothing. */

bool pure_save_enabled(const PureApp *);
/* A text state of m: its stored value, or its initial constant when none. */
PureResult pure_save_open_text(PureApp *, const PureModule *m, uint32_t state, PureDraft **out, PureVersion *saved);
/* A list state of m: its stored items, fields moved by name when the stored
 * record differs from m's. */
PureResult pure_save_open_list(PureApp *, const PureModule *m, uint32_t state, PureList **out, PureDraft **draft,
                               PureVersion *saved);
/* Open the stored numbers, cursors and indices, and put each stored one of the
 * same name and kind that still fits into values, for the states of m that
 * kept marks as new (-1); texts and lists are m's. */
PureResult pure_save_open_values(PureApp *, const PureModule *m, double *values, const int *kept,
                                 PureDraft *const *texts, PureList *const *lists);
/* Store a list whose record changed in its draft, header and items anew. */
PureResult pure_save_rewrite_list(const PureModule *m, uint32_t state, PureList *list, PureDraft *draft);
/* Publish the documents and every state changed since the last save
 * (values are the states' numbers); revisions are the documents'. */
PureResult pure_save_commit(PureApp *, const double *values, void *const *documents, size_t document_count,
                            int64_t *revisions);

/* A list with record's fields, taking from `from` each field of the same name
 * and kind and giving the others their defaults (module.c). */
PureResult pure_list_migrate(PureList **out, const PureList *from, const char *const *names,
                             const uint8_t *text, unsigned count, const PureModule *m, uint32_t record);
#endif
