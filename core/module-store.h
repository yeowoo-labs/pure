#ifndef PURE_MODULE_STORE_H
#define PURE_MODULE_STORE_H
#include "module.h"
/* The app's states on the selected tree store, each save one commit: the app holds the store at path as a handle
 * while it runs (it does not start, PURE_BUSY, on a store another process writes). */
PureData pure_tree_data(const char *path);
/* The same through a store handle (state.h) the host holds: opening a state reads no file. The handle outlives the
 * app. */
PureData pure_store_data(PureStore *);
#endif
