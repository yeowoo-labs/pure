#ifndef PURE_MODULE_STORE_H
#define PURE_MODULE_STORE_H
#include "module.h"
/* Keyed module collections on the selected tree store: one event opens the
 * message key as a held draft and commits it. The path outlives the app. */
PureData pure_tree_data(const char *path);
#endif
