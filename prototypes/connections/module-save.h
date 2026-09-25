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
/* Open the stored numbers, cursors and indices of m and, with load, put each
 * stored one of the same name and kind that still fits into values. */
PureResult pure_save_open_values(PureApp *, const PureModule *m, double *values, bool load);
/* Store a list whose record changed in its draft, header and items anew. */
PureResult pure_save_rewrite_list(const PureModule *m, uint32_t state, PureList *list, PureDraft *draft);
/* Publish every state changed since the last save (values are the pending
 * numbers), with document when not NULL; revision is the document's. */
PureResult pure_save_commit(PureApp *, const double *values, void *document, int64_t *revision);

/* A list with record's fields, taking from `from` each field of the same name
 * and kind and giving the others their defaults (module.c). */
PureResult pure_list_migrate(PureList **out, const PureList *from, const char *const *names,
                             const uint8_t *text, unsigned count, const PureModule *m, uint32_t record);
#endif
